#include "render/Silhouette.h"

#include "render/Modules.h"

#include "core/JsonKeys.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>

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
    { Form::Lattice, "lattice" }, { Form::Band, "band" },       { Form::Arc, "arc" },
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

// --- the sphere (#166) -----------------------------------------------------------------
//
// One convention, used by both directions below and stated once: the body's north pole is
// tipped `tilt` toward the viewer, screen y points down, and a point is visible when it
// faces the viewer (z > 0). Body coordinates first, then a turn about the screen's x axis.

struct OnSphere
{
    float x, y;  // on screen, in units of the body's radius
    float z;     // how squarely it faces the viewer: 1 dead ahead, 0 on the limb, <0 behind
};

OnSphere Project(float latDeg, float lonDeg, float tiltDeg)
{
    const float phi = latDeg * DEG2RAD, lam = lonDeg * DEG2RAD, i = tiltDeg * DEG2RAD;
    const float bx = std::cos(phi) * std::sin(lam);
    const float by = std::sin(phi);
    const float bz = std::cos(phi) * std::cos(lam);
    const float vy = by * std::cos(i) - bz * std::sin(i);
    const float vz = by * std::sin(i) + bz * std::cos(i);
    return { bx, -vy, vz };
}

// The visible part of a latitude band, as quads in units of the body's radius.
//
// Computed rather than drawn, and computed column by column because a band is nearly
// horizontal: a column crosses it almost square-on, so a modest number of them traces its
// edges cleanly, where rows would run nearly parallel to an edge and step along it.
//
// In one column at screen x the visible arc has radius rho = sqrt(1 - x^2). Parametrised by
// theta from the bottom of the column (-90) to the top (+90), the point's height in body
// coordinates is rho * sin(theta + tilt): it rises to rho and then, past the pole, falls a
// little. So a band is at most two runs per column -- the near side, and over the top the
// sliver of the far polar region a tipped planet shows -- and each is solved exactly with an
// arcsine. Nothing is clipped, because nothing is ever outside the limb to begin with.
void BandQuads(float latLo, float latHi, float tiltDeg, int columns, std::vector<Vector2>& out)
{
    const float i = tiltDeg * DEG2RAD;
    const float s1 = std::sin(latLo * DEG2RAD), s2 = std::sin(latHi * DEG2RAD);

    struct Run
    {
        bool  on = false;
        float y0 = 0.0f, y1 = 0.0f;
    };
    // Two runs per column: [0] the rising branch, [1] the falling one.
    std::vector<Run> prev(2), cur(2);
    float            prevX = 0.0f;

    for (int k = 0; k <= columns; k++)
    {
        // Spaced by angle rather than evenly in x, so the columns crowd toward the limb,
        // which is exactly where a band's ends curve hardest.
        const float t = -0.5f * PI + PI * (float)k / (float)columns;
        const float x = std::sin(t);
        const float rho = std::cos(t);

        cur[0] = cur[1] = Run{};
        if (rho > 1e-4f)
        {
            const float a1 = std::fmax(-1.0f, std::fmin(1.0f, s1 / rho));
            const float a2 = std::fmax(-1.0f, std::fmin(1.0f, s2 / rho));
            const float uLo = i - 0.5f * PI, uHi = i + 0.5f * PI;  // u = theta + tilt
            const float rise0 = std::asin(a1), rise1 = std::asin(a2);
            const float fall0 = PI - std::asin(a2), fall1 = PI - std::asin(a1);
            const float spans[2][2] = { { rise0, rise1 }, { fall0, fall1 } };
            for (int b = 0; b < 2; b++)
            {
                const float u0 = std::fmax(spans[b][0], uLo);
                const float u1 = std::fmin(spans[b][1], uHi);
                if (u1 - u0 <= 1e-5f)
                    continue;
                // y = -rho * sin(theta), theta = u - tilt.
                cur[b] = { true, -rho * std::sin(u0 - i), -rho * std::sin(u1 - i) };
            }
        }

        if (k > 0)
            for (int b = 0; b < 2; b++)
                if (prev[b].on && cur[b].on)
                {
                    out.push_back({ prevX, prev[b].y0 });
                    out.push_back({ prevX, prev[b].y1 });
                    out.push_back({ x, cur[b].y1 });
                    out.push_back({ x, cur[b].y0 });
                }
        prev = cur;
        prevX = x;
    }
}

float Smooth01(float e0, float e1, float v)
{
    const float t = std::fmax(0.0f, std::fmin(1.0f, (v - e0) / (e1 - e0)));
    return t * t * (3.0f - 2.0f * t);
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
        case Form::Arc:
        case Form::Polygon: return p.radius;
        case Form::Band: return p.bodyRadius;
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
        case Form::Arc:
        case Form::Polygon:
        case Form::Band: return { 0.0f, 0.0f };
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

std::vector<Vector2> SurfaceOutline(const Piece& p, int segments)
{
    std::vector<Vector2> poly;
    if (!p.surface || p.form != Form::Disc || p.radius <= 0.0f || p.bodyRadius <= 0.0f)
        return poly;

    // The ellipse: its short axis along `angle`, shortened by how obliquely it is seen.
    const int   n = segments < 8 ? 8 : segments;
    const float shortR = p.radius * std::fmax(0.02f, std::fmin(1.0f, p.squash));
    const float a = p.angle * DEG2RAD, cs = std::cos(a), sn = std::sin(a);
    poly.reserve((size_t)n + 8);
    bool        outside = false;
    const float inner = p.bodyRadius * std::cos(PI / (float)BODY_SIDES);  // the polygon's flats
    for (int k = 0; k < n; k++)
    {
        const float   t = 2.0f * PI * (float)k / (float)n;
        const float   u = std::cos(t) * shortR, v = std::sin(t) * p.radius;
        const Vector2 pt{ p.pos.x + u * cs - v * sn, p.pos.y + u * sn + v * cs };
        const float   dx = pt.x - p.bodyPos.x, dy = pt.y - p.bodyPos.y;
        outside = outside || dx * dx + dy * dy > inner * inner;
        poly.push_back(pt);
    }
    if (!outside)
        return poly;

    // Cut by each side of the body's polygon in turn (Sutherland-Hodgman). Both are convex,
    // so the result is too, and a side the ellipse does not reach leaves it as it was.
    std::vector<Vector2> next;
    for (int k = 0; k < BODY_SIDES && !poly.empty(); k++)
    {
        const float   mid = 2.0f * PI * ((float)k + 0.5f) / (float)BODY_SIDES;
        const Vector2 normal{ std::cos(mid), std::sin(mid) };
        auto          over = [&](Vector2 q)
        { return (q.x - p.bodyPos.x) * normal.x + (q.y - p.bodyPos.y) * normal.y - inner; };
        next.clear();
        for (size_t i = 0; i < poly.size(); i++)
        {
            const Vector2 cur = poly[i], nxt = poly[(i + 1) % poly.size()];
            const float   dc = over(cur), dn = over(nxt);
            if (dc <= 0.0f)
                next.push_back(cur);
            if ((dc <= 0.0f) != (dn <= 0.0f))
            {
                const float s = dc / (dc - dn);
                next.push_back({ cur.x + (nxt.x - cur.x) * s, cur.y + (nxt.y - cur.y) * s });
            }
        }
        poly.swap(next);
    }
    return poly;
}

Vector2 SurfaceFanCentre(const Piece& p, const std::vector<Vector2>& outline)
{
    // Inside a convex polygon is on the same side of every edge.
    bool  inside = outline.size() >= 3;
    float sign = 0.0f;
    for (size_t k = 0; k < outline.size() && inside; k++)
    {
        const Vector2 a = outline[k], b = outline[(k + 1) % outline.size()];
        const float   cross = (b.x - a.x) * (p.pos.y - a.y) - (b.y - a.y) * (p.pos.x - a.x);
        if (sign == 0.0f)
            sign = cross;
        inside = cross * sign >= 0.0f;
    }
    if (inside || outline.empty())
        return p.pos;
    Vector2 mid{ 0.0f, 0.0f };
    for (const Vector2& v : outline)
        mid = { mid.x + v.x, mid.y + v.y };
    return { mid.x / (float)outline.size(), mid.y / (float)outline.size() };
}

float EllipseReach(const Piece& p, Vector2 at)
{
    if (p.radius <= 0.0f)
        return 1.0f;
    const float shortR = p.radius * std::fmax(0.02f, std::fmin(1.0f, p.squash));
    const float a = p.angle * DEG2RAD, cs = std::cos(a), sn = std::sin(a);
    const float dx = at.x - p.pos.x, dy = at.y - p.pos.y;
    const float u = (dx * cs + dy * sn) / shortR, v = (-dx * sn + dy * cs) / p.radius;
    return std::fmin(1.0f, std::sqrt(u * u + v * v));
}

float SurfaceFacing(Vector2 offset, Vector2 lightDir)
{
    if (lightDir.x * lightDir.x + lightDir.y * lightDir.y < 0.0001f)
        return 1.0f;
    // As hull.fs: the normal of the sphere under the point, against the light raised a
    // little out of the picture, so the terminator curves round the body.
    const float r2 = std::fmin(1.0f, offset.x * offset.x + offset.y * offset.y);
    const float z = std::fmax(0.001f, std::sqrt(1.0f - r2));
    const float nl = std::sqrt(r2 + z * z);
    const float ll = std::sqrt(lightDir.x * lightDir.x + lightDir.y * lightDir.y + 0.55f * 0.55f);
    const float facing = (offset.x * lightDir.x + offset.y * lightDir.y + z * 0.55f) / (nl * ll);
    return std::fmax(0.0f, facing);
}

static bool ParseShapeUnguarded(const json& j, Shape& out, std::string& error);

std::vector<const ModuleVariant*> AllowedVariants(const Module&                   m,
                                                  const std::vector<std::string>& only,
                                                  const std::vector<std::string>& except)
{
    std::vector<const ModuleVariant*> out;
    for (const ModuleVariant& v : m.variants)
    {
        const bool listed = std::find(only.begin(), only.end(), v.id) != only.end();
        const bool barred = std::find(except.begin(), except.end(), v.id) != except.end();
        if ((only.empty() || listed) && !barred)
            out.push_back(&v);
    }
    return out;
}

// "variants": [...] and "except": [...], on a module part or a kit line: which of a module's
// variants the seed may choose from (#240). Every name has to be a variant of one of
// `modules` -- of the module itself, or of any module carrying the tag a kit line names --
// and something has to be left to choose: a list that rules out everything is a mistake,
// not an empty object.
static bool ParseVariantLists(const json& e, const std::vector<const Module*>& modules,
                              const std::string& what, std::string& variant,
                              std::vector<std::string>& only, std::vector<std::string>& except,
                              std::string& error)
{
    auto names = [&](const char* key, std::vector<std::string>& out)
    {
        if (!e.contains(key))
            return true;
        const json& l = e[key];
        if (!l.is_array() || l.empty())
        {
            error = what + ": \"" + key + "\" is a list of variant ids";
            return false;
        }
        for (const json& n : l)
        {
            if (!n.is_string())
            {
                error = what + ": \"" + key + "\" is a list of variant ids";
                return false;
            }
            const std::string id = n.get<std::string>();
            bool              known = false;
            for (const Module* m : modules)
                for (const ModuleVariant& v : m->variants)
                    known = known || v.id == id;
            if (!known)
            {
                error = what + " has no variant '" + id + "'";
                return false;
            }
            out.push_back(id);
        }
        return true;
    };
    if (!names("variants", only) || !names("except", except))
        return false;
    if (!variant.empty() && (!only.empty() || !except.empty()))
    {
        error = what + ": \"variant\" pins one, \"variants\" and \"except\" choose among them; "
                       "not both";
        return false;
    }
    if (!only.empty() || !except.empty())
    {
        bool left = false;
        for (const Module* m : modules)
            left = left || !AllowedVariants(*m, only, except).empty();
        if (!left)
        {
            error = what + ": \"variants\" and \"except\" leave no variant to choose";
            return false;
        }
    }
    return true;
}

// "kit": { "symmetry": "bilateral", "plain": 0.4, "modules": [
//          { "of": "hatch", "count": [2, 4], "on": "edge" }, { "of": "#light", ... } ] }
// A name that is no module and a tag no module carries are load errors, like a misspelt
// module anywhere else.
static bool ParseKit(const json& k, Kit& kit, std::string& error)
{
    if (!k.is_object() || !OnlyKnownKeys(k, { "symmetry", "plain", "modules" }, error))
    {
        if (error.empty())
            error = "\"kit\" is { \"symmetry\", \"plain\", \"modules\": [...] }";
        return false;
    }
    kit.symmetry = k.value("symmetry", kit.symmetry);
    if (kit.symmetry != "bilateral" && kit.symmetry != "radial" && kit.symmetry != "none")
    {
        error = "kit symmetry is \"bilateral\", \"radial\" or \"none\"";
        return false;
    }
    kit.plain = k.value("plain", kit.plain);
    if (!k.contains("modules") || !k["modules"].is_array())
    {
        error = "a kit needs \"modules\": [...]";
        return false;
    }
    for (const json& m : k["modules"])
    {
        if (!m.is_object() ||
            !OnlyKnownKeys(m,
                           { "of", "count", "on", "scale", "turn", "variant", "variants", "except",
                             "in", "mount", "z", "when", "prefer" },
                           error))
        {
            if (error.empty())
                error = "a kit line is { \"of\", \"count\", \"on\", ... }";
            return false;
        }
        KitEntry e;
        e.of = m.value("of", std::string());
        e.byTag = !e.of.empty() && e.of[0] == '#';
        if (e.byTag)
            e.of = e.of.substr(1);
        std::vector<const Module*> of;
        for (const Module& mod : Modules::All())
            if (e.byTag ? std::find(mod.tags.begin(), mod.tags.end(), e.of) != mod.tags.end()
                        : mod.id == e.of)
                of.push_back(&mod);
        if (of.empty())
        {
            error = std::string(e.byTag ? "no module carries the tag '" : "unknown module '") +
                    e.of + "' in the kit";
            return false;
        }
        if (m.contains("count"))
        {
            const json& c = m["count"];
            if (c.is_number())
                e.lo = e.hi = c.get<float>();
            else if (c.is_array() && c.size() == 2)
            {
                e.lo = c[0].get<float>();
                e.hi = c[1].get<float>();
            }
        }
        e.on = m.value("on", std::string());
        if (!e.on.empty() && e.on != "edge" && e.on != "end" && e.on != "top" && e.on != "ring" &&
            e.on != "middle")
        {
            error = "a kit line's \"on\" is edge, end, top, ring or middle";
            return false;
        }
        e.scale = m.value("scale", 0.0f);
        e.turn = m.value("turn", 0.0f);
        e.variant = m.value("variant", std::string());
        if (!ParseVariantLists(m, of, std::string("kit line '") + (e.byTag ? "#" : "") + e.of + "'",
                               e.variant, e.variants, e.except, error))
            return false;
        e.in = m.value("in", -1);
        e.mount = m.value("mount", e.mount);
        if (e.mount != "on" && e.mount != "out" && e.mount != "centre")
        {
            error = "a kit line's \"mount\" is on, out or centre";
            return false;
        }
        e.z = m.value("z", e.z);
        e.when = m.value("when", std::string());
        e.prefer = m.value("prefer", e.prefer);
        if (e.prefer != "out" && e.prefer != "in")
        {
            error = "a kit line's \"prefer\" is out or in";
            return false;
        }
        kit.entries.push_back(e);
    }
    return true;
}

// Data is written by hand, and a value of the wrong kind -- a range where only a number is
// read, a string for a colour -- is a load error that says so, never an exception that
// takes the program down with nothing named.
bool ParseShape(const json& j, Shape& out, std::string& error)
{
    try
    {
        return ParseShapeUnguarded(j, out, error);
    }
    catch (const json::exception& e)
    {
        error = std::string("a value of the wrong kind: ") + e.what();
        return false;
    }
}

static bool ParseShapeUnguarded(const json& j, Shape& out, std::string& error)
{
    error.clear();

    // Either a bare list of parts, which is what every shape was until the body had
    // properties of its own, or an object carrying the list beside them. The bare form
    // stays valid because a station has no axis and should not have to say so.
    Shape       s;
    const json* parts = &j;
    json        combined;
    size_t      sectionCount = 0;
    size_t      partIndex = 0;
    if (j.is_object())
    {
        if (!OnlyKnownKeys(j, { "tilt", "parts", "vars", "sections", "kit" }, error))
            return false;
        s.axisTilt = j.value("tilt", s.axisTilt);
        if (j.contains("vars"))
        {
            const json& vs = j["vars"];
            if (!vs.is_object())
            {
                error = "\"vars\" is { \"name\": [min, max] or [[r, g, b], ...] }";
                return false;
            }
            for (auto it = vs.begin(); it != vs.end(); ++it)
            {
                Shape::Var var;
                var.name = it.key();
                const json& v = it.value();
                if (v.is_array() && v.size() == 2 && v[0].is_number() && v[1].is_number())
                {
                    var.lo = v[0].get<float>();
                    var.hi = v[1].get<float>();
                }
                else if (v.is_array() && !v.empty() && v[0].is_array())
                {
                    for (const json& c : v)
                    {
                        if (!c.is_array() || c.size() < 3)
                        {
                            error = "variable '" + var.name + "': a colour is [r, g, b]";
                            return false;
                        }
                        var.palette.push_back({ (unsigned char)c[0].get<int>(),
                                                (unsigned char)c[1].get<int>(),
                                                (unsigned char)c[2].get<int>(), 255 });
                    }
                }
                else
                {
                    error = "variable '" + var.name +
                            "' is [min, max] or a list of colours [[r, g, b], ...]";
                    return false;
                }
                s.vars.push_back(var);
            }
        }
        if (!j.contains("parts"))
        {
            error = "a shape object needs \"parts\"";
            return false;
        }
        parts = &j["parts"];
        if (j.contains("sections"))
        {
            // Sections are parts that come first and carry sockets: read in the same loop.
            if (!j["sections"].is_array() || !parts->is_array())
            {
                error = "\"sections\" is an array of parts";
                return false;
            }
            sectionCount = j["sections"].size();
            combined = j["sections"];
            for (const json& e : *parts)
                combined.push_back(e);
            parts = &combined;
        }
        if (j.contains("kit") && !ParseKit(j["kit"], s.kit, error))
            return false;
    }
    if (!parts->is_array())
    {
        error = "a shape is an array of parts";
        return false;
    }

    for (const json& e : *parts)
    {
        const bool isSection = partIndex++ < sectionCount;
        if (!e.is_object())
        {
            error = "a part is an object";
            return false;
        }
        // A misspelled field would be read as absent and draw the default (#191), which
        // for a part is the kind of wrong nobody notices until it is the only one left.
        if (!OnlyKnownKeys(e,
                           { "form",
                             "role",
                             "at",
                             "sides",
                             "angle",
                             "radius",
                             "width",
                             "length",
                             "count",
                             "filled",
                             "repeat",
                             "mirror",
                             "minPixels",
                             "jitterAngle",
                             "jitterScale",
                             "alpha",
                             "orbitRadius",
                             "orbitPeriod",
                             "orbitPhase",
                             "orbitTilt",
                             "lat",
                             "lon",
                             "spin",
                             "blink",
                             "onlyThrusting",
                             "tint",
                             "from",
                             "to",
                             "row",
                             "module",
                             "variant",
                             "variants",
                             "except",
                             "scale",
                             "chance",
                             "group",
                             "pivot",
                             "onlyDark",
                             "tip",
                             "jagged",
                             "soft",
                             "pitch",
                             "z" },
                           error))
            return false;
        Part p;
        if (e.contains("module"))
        {
            // A module's name must mean something now, at load: a part that silently drew
            // nothing because its module was misspelt is the kind of wrong nobody notices.
            p.module = e.value("module", std::string());
            p.variant = e.value("variant", std::string());
            const Module* m = Modules::Find(p.module);
            if (m == nullptr)
            {
                error = "unknown module '" + p.module + "'";
                return false;
            }
            bool found = p.variant.empty();
            for (const ModuleVariant& v : m->variants)
                found = found || v.id == p.variant;
            if (!found)
            {
                error = "module '" + p.module + "' has no variant '" + p.variant + "'";
                return false;
            }
            if (!ParseVariantLists(e, { m }, "module '" + p.module + "'", p.variant, p.variants,
                                   p.except, error))
                return false;
        }
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

        // A shape variable by name, "$name", or -1 having said why not.
        // "$name", or a straight line of one: "-$a", "$w*0.5", "$r+0.05", "-$len/2+0.1".
        // Enough for a pair of jaws that open together, a rim a fixed step outside its
        // crater, a pivot at the end of a ranged length -- and no more: a shape is data, not
        // a program. The step may be a range, "$r+[0.02,0.06]", which this part rolls on
        // its own: a shared size, a step outside it that differs from part to part. Sets
        // the slope and the offset's range; -1 having said why not.
        auto variable = [&](const json& v, float& mul, float& add, float& addHi) -> int
        {
            const std::string text = v.get<std::string>();
            size_t            at = 0;
            mul = 1.0f;
            add = 0.0f;
            addHi = 0.0f;
            if (text[at] == '-')
            {
                mul = -1.0f;
                at++;
            }
            at++;  // the '$'
            size_t end = at;
            while (end < text.size() &&
                   (std::isalnum((unsigned char)text[end]) || text[end] == '_'))
                end++;
            const std::string name = text.substr(at, end - at);
            int               k = -1;
            for (size_t i = 0; i < s.vars.size(); i++)
                if (s.vars[i].name == name)
                    k = (int)i;
            if (k < 0)
            {
                error = "unknown variable '$" + name + "' (declare it in the shape's \"vars\")";
                return -1;
            }
            const char* rest = text.c_str() + end;
            char*       stop = nullptr;
            if (*rest == '*' || *rest == '/')
            {
                const char  op = *rest;
                const float x = std::strtof(rest + 1, &stop);
                if (stop == rest + 1 || (op == '/' && x == 0.0f))
                {
                    error = "'" + text + "': a number was expected after '" + op + "'";
                    return -1;
                }
                mul *= op == '*' ? x : 1.0f / x;
                rest = stop;
            }
            if ((*rest == '+' || *rest == '-') && rest[1] == '[')
            {
                const float sign = *rest == '-' ? -1.0f : 1.0f;
                const float a = std::strtof(rest + 2, &stop);
                bool        ok = stop != rest + 2 && *stop == ',';
                const char* second = ok ? stop + 1 : rest;
                const float b = ok ? std::strtof(second, &stop) : 0.0f;
                ok = ok && stop != second && *stop == ']';
                if (!ok)
                {
                    error = "'" + text + "': a range is [min,max] after the sign";
                    return -1;
                }
                add = sign * a;
                addHi = sign * b;
                rest = stop + 1;
            }
            else if (*rest == '+' || *rest == '-')
            {
                add = std::strtof(rest, &stop);
                if (stop == rest + 1)
                {
                    error = "'" + text + "': a number was expected after the sign";
                    return -1;
                }
                addHi = add;
                rest = stop;
            }
            else
                addHi = add;
            if (*rest != '\0')
            {
                error = "'" + text +
                        "' is \"$name\", optionally negated, times or over a "
                        "number, plus or minus a number or a [min,max] range";
                return -1;
            }
            return k;
        };
        auto isVariable = [](const json& v)
        {
            if (!v.is_string())
                return false;
            const std::string t = v.get<std::string>();
            return (!t.empty() && t[0] == '$') || (t.size() > 1 && t[0] == '-' && t[1] == '$');
        };

        // A number, or [min, max] for the seed to choose in (#240), or "$name" for a shape
        // variable's roll.
        auto num = [&](const json& v, float& dst, Part::Field f) -> bool
        {
            if (isVariable(v))
            {
                float     mul = 1.0f, add = 0.0f, addHi = 0.0f;
                const int k = variable(v, mul, add, addHi);
                if (k < 0)
                    return false;
                if (!s.vars[k].palette.empty())
                {
                    error = "variable '" + s.vars[k].name + "' is a colour, not a number";
                    return false;
                }
                // A line of a uniform roll is a uniform roll between the line's ends.
                const float lo = s.vars[k].lo * mul, hi = s.vars[k].hi * mul;
                dst = lo + add;
                p.vary.push_back({ f, lo, hi, k, add, addHi });
                return true;
            }
            if (v.is_number())
            {
                dst = v.get<float>();
                return true;
            }
            if (v.is_array() && v.size() == 2 && v[0].is_number() && v[1].is_number())
            {
                dst = v[0].get<float>();
                p.vary.push_back({ f, v[0].get<float>(), v[1].get<float>() });
                return true;
            }
            error = "a number or [min, max] was expected";
            return false;
        };
        auto field = [&](const char* key, float& dst, Part::Field f) -> bool
        { return !e.contains(key) || num(e[key], dst, f); };
        if (e.contains("at") && e["at"].is_array() && e["at"].size() == 2)
            if (!num(e["at"][0], p.at.x, Part::Field::AtX) ||
                !num(e["at"][1], p.at.y, Part::Field::AtY))
                return false;

        float sides = (float)p.sides, count = (float)p.count;
        if (!field("sides", sides, Part::Field::Sides) ||
            !field("angle", p.angle, Part::Field::Angle) ||
            !field("radius", p.radius, Part::Field::Radius) ||
            !field("width", p.width, Part::Field::Width) ||
            !field("length", p.length, Part::Field::Length) ||
            !field("count", count, Part::Field::Count) ||
            !field("alpha", p.alpha, Part::Field::Alpha) ||
            !field("scale", p.scale, Part::Field::Scale))
            return false;
        p.sides = (int)sides;
        p.count = (int)count;
        p.chance = e.value("chance", p.chance);
        if (e.contains("group"))
        {
            // A group is a variable nobody can name in data: "#" cannot start a "$" name.
            const std::string name = "#" + e.value("group", std::string());
            p.chanceVar = -1;
            for (size_t k = 0; k < s.vars.size(); k++)
                if (s.vars[k].name == name)
                    p.chanceVar = (int)k;
            if (p.chanceVar < 0)
            {
                Shape::Var g;
                g.name = name;
                g.lo = 0.0f;
                g.hi = 1.0f;
                s.vars.push_back(g);
                p.chanceVar = (int)s.vars.size() - 1;
            }
        }
        if (e.contains("pivot"))
        {
            const json& pv = e["pivot"];
            if (!pv.is_array() || pv.size() != 2)
            {
                error = "\"pivot\" is [x, y]";
                return false;
            }
            if (!num(pv[0], p.pivot.x, Part::Field::PivotX) ||
                !num(pv[1], p.pivot.y, Part::Field::PivotY))
                return false;
            p.hasPivot = true;
        }
        p.filled = e.value("filled", p.filled);
        p.repeat = e.value("repeat", p.repeat);
        p.mirror = e.value("mirror", p.mirror);
        p.minPixels = e.value("minPixels", p.minPixels);
        p.jitterAngle = e.value("jitterAngle", p.jitterAngle);
        p.jitterScale = e.value("jitterScale", p.jitterScale);
        p.orbitRadius = e.value("orbitRadius", p.orbitRadius);
        p.orbitPeriod = e.value("orbitPeriod", p.orbitPeriod);
        p.orbitPhase = e.value("orbitPhase", p.orbitPhase);
        p.orbitTilt = e.value("orbitTilt", p.orbitTilt);
        p.surface = p.form == Form::Band || e.contains("lat") || e.contains("lon");
        if (!field("lat", p.lat, Part::Field::Lat) || !field("lon", p.lon, Part::Field::Lon))
            return false;
        if (!field("spin", p.spin, Part::Field::Spin) ||
            !field("blink", p.blink, Part::Field::Blink) ||
            !field("from", p.arcFrom, Part::Field::ArcFrom) ||
            !field("to", p.arcTo, Part::Field::ArcTo))
            return false;
        p.onlyThrusting = e.value("onlyThrusting", p.onlyThrusting);
        p.onlyDark = e.value("onlyDark", p.onlyDark);
        p.soft = e.value("soft", p.soft);
        p.section = isSection;
        p.pitch = e.value("pitch", p.pitch);
        p.z = e.value("z", p.z);
        if (!field("tip", p.tip, Part::Field::Tip) ||
            !field("jagged", p.jagged, Part::Field::Jagged))
            return false;
        if (e.contains("tint"))
        {
            // A colour, or a list of colours for the seed to pick from (#240).
            const json& t = e["tint"];
            if (isVariable(t))
            {
                float     mul = 1.0f, add = 0.0f, addHi = 0.0f;
                const int k = variable(t, mul, add, addHi);
                if (k < 0)
                    return false;
                if (s.vars[k].palette.empty())
                {
                    error = "variable '" + s.vars[k].name + "' is a number, not a colour";
                    return false;
                }
                p.palette = s.vars[k].palette;
                p.tint = p.palette.front();
                p.tintVar = k;
            }
            auto colour = [](const json& c, Color& out)
            {
                if (!c.is_array() || c.size() < 3 || !c[0].is_number())
                    return false;
                out = { (unsigned char)c[0].get<int>(), (unsigned char)c[1].get<int>(),
                        (unsigned char)c[2].get<int>(), 255 };
                return true;
            };
            if (p.tintVar >= 0)
            {
            }
            else if (t.is_array() && !t.empty() && t[0].is_array())
            {
                for (const json& c : t)
                {
                    Color col;
                    if (!colour(c, col))
                    {
                        error = "\"tint\" is a colour [r, g, b] or a list of them";
                        return false;
                    }
                    p.palette.push_back(col);
                }
                p.tint = p.palette.front();
            }
            else if (!colour(t, p.tint))
            {
                error = "\"tint\" is a colour [r, g, b] or a list of them";
                return false;
            }
        }
        if (e.contains("row"))
        {
            const json& r = e["row"];
            if (!r.is_object() ||
                !OnlyKnownKeys(
                    r,
                    { "count", "step", "centred", "turn", "taper", "ring", "spread", "taperStep" },
                    error) ||
                (!r.contains("ring") &&
                 (!r.contains("step") || !r["step"].is_array() || r["step"].size() != 2)))
            {
                if (error.empty())
                    error = "\"row\" is { \"count\": n, \"step\": [dx, dy] } or "
                            "{ \"count\": n, \"ring\": radius }";
                return false;
            }
            float rc = 1.0f;
            if (r.contains("count") && !num(r["count"], rc, Part::Field::RowCount))
                return false;
            p.rowCount = (int)rc;
            if (r.contains("step") && (!num(r["step"][0], p.rowStep.x, Part::Field::StepX) ||
                                       !num(r["step"][1], p.rowStep.y, Part::Field::StepY)))
                return false;
            if ((r.contains("ring") && !num(r["ring"], p.rowRing, Part::Field::RowRing)) ||
                (r.contains("spread") && !num(r["spread"], p.rowSpread, Part::Field::RowSpread)))
                return false;
            p.rowCentred = r.value("centred", false);
            p.rowTaperStep = r.value("taperStep", false);
            if ((r.contains("turn") && !num(r["turn"], p.rowTurn, Part::Field::RowTurn)) ||
                (r.contains("taper") && !num(r["taper"], p.rowTaper, Part::Field::RowTaper)))
                return false;
        }
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
        // The far end of a row is as far as the part reaches.
        const float endX = p.at.x + p.rowStep.x * (float)(std::max(1, p.rowCount) - 1);
        const float endY = p.at.y + p.rowStep.y * (float)(std::max(1, p.rowCount) - 1);
        const float from = std::fmax(std::sqrt(p.at.x * p.at.x + p.at.y * p.at.y),
                                     std::sqrt(endX * endX + endY * endY));

        // Only the measurements this form actually uses. Every part carries a default for
        // all of them, so taking the largest would have an arm reaching a full radius past
        // its own end on the strength of a `radius` it never reads.
        float own = 0.0f;
        switch (p.form)
        {
            case Form::Disc:
            case Form::Ring:
            case Form::Arc:
            case Form::Polygon: own = p.radius; break;
            case Form::Band: own = 1.0f; break;
            case Form::Capsule:
            case Form::Chevron:
            case Form::Bar:
            case Form::Lattice: own = std::fmax(p.length, p.width) * 0.5f; break;
        }
        // A module is as big as its scale, not as its unused `radius` (#240): read as a
        // part, every rock in a belt reached a whole object radius past its place.
        if (!p.module.empty())
            own = p.scale;
        // A surface part sits on the unit sphere whatever its `at` says.
        if (p.surface)
        {
            reach = std::fmax(reach, 1.0f);
            continue;
        }
        reach = std::fmax(reach, (from + own) * (1.0f + p.jitterScale));
    }
    return reach;
}

std::vector<float> RollVars(const Shape& s, int seed, int salt)
{
    std::vector<float> rolls(s.vars.size());
    for (size_t k = 0; k < s.vars.size(); k++)
        rolls[k] = Hash01(seed, salt + 17 + (int)k * 53);
    return rolls;
}

bool Resolve(const Part& p, int seed, int salt, Part& out, const float* rolls)
{
    out = p;
    if (p.chance < 1.0f)
    {
        const float u =
            (p.chanceVar >= 0 && rolls != nullptr) ? rolls[p.chanceVar] : Hash01(seed, salt + 911);
        if (u >= p.chance)
            return false;
    }
    for (size_t k = 0; k < p.vary.size(); k++)
    {
        const Part::Vary& v = p.vary[k];
        const float u = (v.var >= 0 && rolls != nullptr) ? rolls[v.var]
                                                         : Hash01(seed, salt + 701 + (int)k * 17);
        float       x = v.lo + (v.hi - v.lo) * u;
        if (v.var >= 0)
            x += v.plusHi == v.plusLo
                     ? v.plusLo
                     : v.plusLo + (v.plusHi - v.plusLo) * Hash01(seed, salt + 701 + (int)k * 17);
        switch (v.field)
        {
            case Part::Field::Radius: out.radius = x; break;
            case Part::Field::Width: out.width = x; break;
            case Part::Field::Length: out.length = x; break;
            case Part::Field::Angle: out.angle = x; break;
            case Part::Field::AtX: out.at.x = x; break;
            case Part::Field::AtY: out.at.y = x; break;
            case Part::Field::Alpha: out.alpha = x; break;
            case Part::Field::Scale: out.scale = x; break;
            case Part::Field::RowCount: out.rowCount = (int)std::lround(x); break;
            case Part::Field::Sides: out.sides = (int)std::lround(x); break;
            case Part::Field::Count: out.count = (int)std::lround(x); break;
            case Part::Field::Lat: out.lat = x; break;
            case Part::Field::Lon: out.lon = x; break;
            case Part::Field::Spin: out.spin = x; break;
            case Part::Field::Blink: out.blink = x; break;
            case Part::Field::ArcFrom: out.arcFrom = x; break;
            case Part::Field::ArcTo: out.arcTo = x; break;
            case Part::Field::StepX: out.rowStep.x = x; break;
            case Part::Field::StepY: out.rowStep.y = x; break;
            case Part::Field::RowTurn: out.rowTurn = x; break;
            case Part::Field::RowTaper: out.rowTaper = x; break;
            case Part::Field::PivotX: out.pivot.x = x; break;
            case Part::Field::PivotY: out.pivot.y = x; break;
            case Part::Field::Tip: out.tip = x; break;
            case Part::Field::Jagged: out.jagged = x; break;
            case Part::Field::RowRing: out.rowRing = x; break;
            case Part::Field::RowSpread: out.rowSpread = x; break;
        }
    }
    if (!p.palette.empty())
    {
        const float u =
            (p.tintVar >= 0 && rolls != nullptr) ? rolls[p.tintVar] : Hash01(seed, salt + 503);
        out.tint = p.palette[std::min(p.palette.size() - 1, (size_t)(u * p.palette.size()))];
    }
    if (out.hasPivot)
    {
        // Turned about the joint: the centre moves by however far the turn carries it.
        const float   c = std::cos(out.angle * DEG2RAD), sn = std::sin(out.angle * DEG2RAD);
        const float   px = out.pivot.x, py = out.pivot.y;
        const Vector2 shift = { px - (px * c - py * sn), py - (px * sn + py * c) };
        if (out.rowRing > 0.0f && out.rowCount > 1)
            out.pivotShift = shift;
        else
            out.at = { out.at.x + shift.x, out.at.y + shift.y };
        out.hasPivot = false;
    }
    // A bent or tapering row is spelled out copy by copy (SpellRow), and centred there.
    if (p.rowCentred && out.rowCount > 1 && out.rowTurn == 0.0f && out.rowTaper == 1.0f)
    {
        // The row's middle where `at` says, whatever count the roll gave it.
        const float half = 0.5f * (float)(out.rowCount - 1);
        out.at = { out.at.x - out.rowStep.x * half, out.at.y - out.rowStep.y * half };
        out.rowCentred = false;
    }
    return true;
}

// A row that bends or tapers, as one part per copy (#240): each copy is placed a step on
// from the last along a direction turned by `rowTurn` per copy, turned with it, and sized
// by `rowTaper` to the power of its place. A straight even row is returned as it is, for
// the composer to lay out as it always has.
std::vector<Part> SpellRow(const Part& p)
{
    if (p.rowCount > 1 && p.rowRing > 0.0f)
    {
        // Round a centre: closed when the spread is a full turn, a fan about the part's own
        // direction otherwise. The part's own offset from a pivot turns with its copy.
        std::vector<Part> copies;
        const int         n = p.rowCount;
        const bool        closed = p.rowSpread >= 359.9f;
        float             size = 1.0f;
        for (int k = 0; k < n; k++)
        {
            const float theta = closed
                                    ? 360.0f * (float)k / (float)n
                                    : -0.5f * p.rowSpread + p.rowSpread * (float)k / (float)(n - 1);
            const float c = std::cos(theta * DEG2RAD), sn = std::sin(theta * DEG2RAD);
            const float lx = p.rowRing + p.pivotShift.x, ly = p.pivotShift.y;
            Part        q = p;
            q.rowCount = 1;
            q.rowStep = { 0.0f, 0.0f };
            q.rowRing = 0.0f;
            q.rowCentred = false;
            q.pivotShift = { 0.0f, 0.0f };
            q.at = { p.at.x + lx * c - ly * sn, p.at.y + lx * sn + ly * c };
            q.angle = p.angle + theta;
            q.radius *= size;
            q.width *= size;
            q.length *= size;
            q.scale *= size;
            copies.push_back(q);
            size *= p.rowTaper;
        }
        return copies;
    }
    if (p.rowCount <= 1 || (p.rowTurn == 0.0f && p.rowTaper == 1.0f))
        return { p };
    std::vector<Part>    copies;
    std::vector<Vector2> offsets;
    Vector2              off = { 0.0f, 0.0f };
    float                size = 1.0f;
    for (int k = 0; k < p.rowCount; k++)
    {
        Part q = p;
        q.rowCount = 1;
        q.rowStep = { 0.0f, 0.0f };
        q.rowCentred = false;
        q.angle = p.angle + p.rowTurn * (float)k;
        q.radius *= size;
        q.width *= size;
        q.length *= size;
        q.scale *= size;
        offsets.push_back(off);
        copies.push_back(q);
        const float t = p.rowTurn * (float)k * DEG2RAD;
        const float c = std::cos(t), sn = std::sin(t);
        // With taperStep the gaps shrink with the copies, so a tapering row stays touching
        // and a bent one spirals inward instead of closing into a circle.
        const float g = p.rowTaperStep ? size : 1.0f;
        off = { off.x + (p.rowStep.x * c - p.rowStep.y * sn) * g,
                off.y + (p.rowStep.x * sn + p.rowStep.y * c) * g };
        size *= p.rowTaper;
    }
    Vector2 shift = { 0.0f, 0.0f };
    if (p.rowCentred)
    {
        for (const Vector2& o : offsets)
            shift = { shift.x + o.x, shift.y + o.y };
        shift = { shift.x / (float)offsets.size(), shift.y / (float)offsets.size() };
    }
    for (size_t k = 0; k < copies.size(); k++)
        copies[k].at = { p.at.x + offsets[k].x - shift.x, p.at.y + offsets[k].y - shift.y };
    return copies;
}

// A module laid on a planet's surface (#240): a base, a city, a crater field. Its own frame
// is a small patch of the sphere at the part's latitude and longitude, measured in the
// body's radius like everything else on it, so an offset becomes degrees -- across a
// latitude by its own width, which keeps a city the same shape near a pole. Each module
// part becomes an ordinary surface part, and the composer then does what it does for any:
// the planet's turn carries it round, the limb squashes it, the far side hides it. Repeat,
// mirror and spin stay on the copies for the same reason, so a repeated base is spread
// round the planet in longitude and a mirrored one is reflected across the equator.
void ExpandOnSphere(const Part& p, const ModuleVariant& v, int seed, int salt, Shape& out)
{
    const int                rows = p.rowCount < 1 ? 1 : p.rowCount;
    const float              ca = std::cos(p.angle * DEG2RAD), sa = std::sin(p.angle * DEG2RAD);
    const std::vector<float> rolls = RollVars(v.shape, seed, salt + 20);
    for (int k = 0; k < rows; k++)
        for (size_t j = 0; j < v.shape.parts.size(); j++)
        {
            Part resolved;
            if (!Resolve(v.shape.parts[j], seed, salt + (int)j * 131, resolved,
                         rolls.empty() ? nullptr : rolls.data()))
                continue;
            for (const Part& mp : SpellRow(resolved))
            {
                // A surface part is one point on the sphere, so a row inside the module is
                // spelled out here rather than left to the composer.
                const int own = mp.rowCount < 1 ? 1 : mp.rowCount;
                for (int n = 0; n < own; n++)
                {
                    const float lx = (mp.at.x + mp.rowStep.x * (float)n) * p.scale;
                    const float ly = (mp.at.y + mp.rowStep.y * (float)n) * p.scale;
                    const float x = p.rowStep.x * (float)k + lx * ca - ly * sa;
                    const float y = p.rowStep.y * (float)k + lx * sa + ly * ca;
                    Part        q = mp;
                    q.surface = true;
                    // Screen y points south, and a degree of longitude narrows towards a pole.
                    q.lat = std::fmax(-89.0f, std::fmin(89.0f, p.lat - y * RAD2DEG));
                    q.lon = p.lon + x / std::fmax(0.2f, std::cos(q.lat * DEG2RAD)) * RAD2DEG;
                    q.at = { 0.0f, 0.0f };
                    q.angle = mp.angle + p.angle;
                    q.radius *= p.scale;
                    q.width *= p.scale;
                    q.length *= p.scale;
                    q.rowCount = 1;
                    q.rowStep = { 0.0f, 0.0f };
                    q.alpha *= p.alpha;
                    if (q.tint.a == 0)
                        q.tint = p.tint;
                    const float seen = mp.minPixels > 0.0f ? mp.minPixels : 10.0f;
                    q.minPixels = std::fmax(p.minPixels, seen / std::fmax(p.scale, 0.001f));
                    q.repeat = p.repeat;
                    q.mirror = p.mirror;
                    q.spin = p.spin;
                    q.z = mp.z + p.z;
                    out.parts.push_back(q);
                }
            }
        }
}

// Every module part of a shape, replaced by the parts of the variant it stands for,
// carried to where each copy of it goes (#240). Repeat, mirror and row place the module
// as a whole; inside it, its own parts keep their arrangement, turned with it, scaled by
// `scale`, reflected when the copy is a mirror image. The result has no module parts left
// and goes through the ordinary composer.
Shape ExpandModules(const Shape& in, const Pose& pose)
{
    Shape out;
    out.axisTilt = in.axisTilt;
    // The object's own variables, rolled once for all its parts.
    const std::vector<float> top = RollVars(in, pose.seed, 9001);
    // Every part as this object has it, and then whatever its kit places on its sections:
    // the kit's modules go through exactly the same expansion as written ones.
    std::vector<std::pair<Part, size_t>> work;
    std::vector<Part>                    sections;
    for (size_t i = 0; i < in.parts.size(); i++)
    {
        Part resolved;
        if (!Resolve(in.parts[i], pose.seed, (int)i * 977 + 3, resolved,
                     top.empty() ? nullptr : top.data()))
            continue;
        for (const Part& p : SpellRow(resolved))
        {
            work.push_back({ p, i });
            if (p.section)
                sections.push_back(p);
        }
    }
    if (!in.kit.entries.empty())
    {
        const std::vector<Part> placed = PlaceKit(in.kit, sections, pose.seed);
        for (size_t k = 0; k < placed.size(); k++)
            work.push_back({ placed[k], in.parts.size() + k });
    }
    for (const auto& item : work)
    {
        const Part&  p = item.first;
        const size_t i = item.second;
        {
            if (p.module.empty())
            {
                out.parts.push_back(p);
                continue;
            }
            const Module* mod = Modules::Find(p.module);
            if (mod == nullptr || mod->variants.empty())
                continue;
            const ModuleVariant* v = nullptr;
            if (!p.variant.empty())
            {
                for (const ModuleVariant& c : mod->variants)
                    if (c.id == p.variant)
                        v = &c;
            }
            else
            {
                // The object's choice, per module part: the same every frame and on every
                // client, different from one object to the next -- among the variants the
                // part allows, which without lists is all of them.
                const std::vector<const ModuleVariant*> allowed =
                    AllowedVariants(*mod, p.variants, p.except);
                const int n = (int)allowed.size();
                if (n > 0)
                    v = allowed[std::min(n - 1, (int)(Hash01(pose.seed, (int)i * 977 + 5) * n))];
            }
            if (v == nullptr)
                continue;

            if (p.surface)
            {
                ExpandOnSphere(p, *v, pose.seed, (int)i * 977 + 41, out);
                continue;
            }

            const int   repeat = p.repeat < 1 ? 1 : p.repeat;
            const int   sides = p.mirror ? 2 : 1;
            const int   rows = p.rowCount < 1 ? 1 : p.rowCount;
            const float turned = (float)std::fmod((double)p.spin * pose.time, 360.0);
            // The module's variables, rolled per placement: two hatches on one hull may differ,
            // the parts of one hatch may not. Every copy of a repeat or row shares the roll.
            const std::vector<float> mrolls = RollVars(v->shape, pose.seed, (int)i * 977 + 61);
            for (int r = 0; r < repeat; r++)
            {
                const float rot = (360.0f / (float)repeat) * (float)r + turned;
                const float cr = std::cos(rot * DEG2RAD), sr = std::sin(rot * DEG2RAD);
                for (int m = 0; m < sides * rows; m++)
                {
                    if (p.mirrorOnly && m % sides == 0)
                        continue;
                    const int     k = m / sides;
                    const float   flip = (m % sides == 0) ? 1.0f : -1.0f;
                    const float   ox = p.at.x + p.rowStep.x * (float)k;
                    const float   oy = (p.at.y + p.rowStep.y * (float)k) * flip;
                    const Vector2 origin = { ox * cr - oy * sr, ox * sr + oy * cr };
                    const float   a = p.angle * flip + rot;
                    const float   ca = std::cos(a * DEG2RAD), sa = std::sin(a * DEG2RAD);
                    for (size_t j = 0; j < v->shape.parts.size(); j++)
                    {
                        // Resolved once per module part, not per copy: a row of the same hatch
                        // is a row of the same hatch, and rhythm is what reads as designed.
                        Part resolved;
                        if (!Resolve(v->shape.parts[j], pose.seed, (int)i * 977 + (int)j * 131 + 41,
                                     resolved, mrolls.empty() ? nullptr : mrolls.data()))
                            continue;
                        for (const Part& mp : SpellRow(resolved))
                        {
                            Part        q = mp;
                            const float lx = mp.at.x * p.scale, ly = mp.at.y * p.scale * flip;
                            q.at = { origin.x + lx * ca - ly * sa, origin.y + lx * sa + ly * ca };
                            q.angle = mp.angle * flip + a;
                            q.radius *= p.scale;
                            q.width *= p.scale;
                            q.length *= p.scale;
                            const float sx = mp.rowStep.x * p.scale,
                                        sy = mp.rowStep.y * p.scale * flip;
                            q.rowStep = { sx * ca - sy * sa, sx * sa + sy * ca };
                            if (flip < 0.0f)
                            {
                                q.arcFrom = -mp.arcTo;
                                q.arcTo = -mp.arcFrom;
                            }
                            q.alpha *= p.alpha;
                            if (q.tint.a == 0)
                                q.tint = p.tint;
                            // Seen when the MODULE is big enough to see, not the object: a hatch
                            // a twentieth of a station appears as you approach it.
                            const float own = mp.minPixels > 0.0f ? mp.minPixels : 10.0f;
                            q.minPixels = std::fmax(p.minPixels, own / std::fmax(p.scale, 0.001f));
                            q.repeat = 1;
                            q.mirror = false;
                            q.spin = 0.0f;
                            q.z = mp.z + p.z;  // the placement's layer, then the module's own order
                            out.parts.push_back(q);
                        }
                    }
                }
            }
        }
    }
    return out;
}

std::vector<Piece> Compose(const Shape& shape, const Pose& pose)
{
    bool hasModules = false;
    for (const Part& p : shape.parts)
        hasModules = hasModules || !p.module.empty();
    bool varies = hasModules;
    for (const Part& p : shape.parts)
        varies = varies || !shape.kit.entries.empty() || !p.vary.empty() || !p.palette.empty() ||
                 p.chance < 1.0f || p.rowCentred || p.rowTurn != 0.0f || p.rowTaper != 1.0f ||
                 p.hasPivot || p.rowRing > 0.0f;
    // Ranges, palettes and chance are settled in the same pass that expands modules, so
    // everything below sees fixed numbers.
    const Shape  expanded = varies ? ExpandModules(shape, pose) : Shape{};
    const Shape& s = varies ? expanded : shape;

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
            const float turned = (float)std::fmod((double)p.spin * pose.time, 360.0);
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
                const float t = (float)std::fmod(pose.time / p.blink + phase, 1.0);
                brightness = p.alpha * (0.25f + 0.75f * (0.5f + 0.5f * std::cos(t * 2.0f * PI)));
            }

            // The part's offset is turned by its repeat step and then by the object's own
            // heading, so a ship's parts follow its nose.
            const float turn = (spin + wobble) * DEG2RAD + pose.heading;
            const float cs = std::cos(turn), sn = std::sin(turn);

            // Once, or twice reflected across the object's own axis -- and each of those
            // once per place along a row (#214).
            const int sides = p.mirror ? 2 : 1;
            const int rows = p.rowCount < 1 ? 1 : p.rowCount;
            for (int m = 0; m < sides * rows; m++)
            {
                const int   k = m / sides;  // place along the row
                const float flip = (m % sides == 0) ? 1.0f : -1.0f;
                const float ax = p.at.x + p.rowStep.x * (float)k;
                const float ay = (p.at.y + p.rowStep.y * (float)k) * flip;

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
                piece.tint = p.tint;
                piece.onlyDark = p.onlyDark;
                piece.z = p.z;
                piece.tip = p.tip;
                piece.jagged = p.jagged;
                piece.jagSeed = (int)(Hash01(seed, salt + 337 + m * 7) * 1000000.0f);
                piece.soft = p.soft;
                // An arc's ends turn with the part, and a mirrored arc runs the other way.
                piece.arcFrom = flip > 0.0f ? piece.angle + p.arcFrom : piece.angle - p.arcTo;
                piece.arcTo = flip > 0.0f ? piece.angle + p.arcTo : piece.angle - p.arcFrom;

                // On the sphere rather than on the disc (#166). The planet's own turn
                // carries a surface part across the face and round the back, so where it is
                // comes from latitude, longitude and the clock -- and on the far side it is
                // simply not there.
                if (p.surface)
                {
                    piece.surface = true;
                    piece.bodyPos = pos;
                    piece.bodyRadius = size;
                    piece.pos = pos;
                    piece.angle = pose.heading * RAD2DEG;
                    piece.depth = 0.0f;

                    // A mirror reflects across the equator; a repeat is spread evenly round
                    // the planet in longitude.
                    const float lat = p.lat * flip;

                    if (p.form == Form::Band)
                    {
                        // A band is the same all the way round, so the planet's turn does not
                        // move it -- which is true of real ones, and is why a gas giant's
                        // *storms* go round and its belts do not.
                        const float          half = 0.5f * std::fabs(p.width);
                        std::vector<Vector2> unit;
                        const int            columns = pixels > 200.0f ? 64 : 36;
                        BandQuads(std::fmax(-90.0f, lat - half), std::fmin(90.0f, lat + half),
                                  s.axisTilt, columns, unit);
                        if (unit.empty())
                            continue;
                        const float ch = std::cos(pose.heading), sh = std::sin(pose.heading);
                        piece.strip.reserve(unit.size());
                        for (const Vector2& u : unit)
                            piece.strip.push_back({ pos.x + (u.x * ch - u.y * sh) * size,
                                                    pos.y + (u.x * sh + u.y * ch) * size });
                        piece.radius = size;
                        out.push_back(piece);
                        continue;
                    }

                    // Where this object's surface starts, seeded per object, so two rocky
                    // planets do not wear their craters in the same places. One offset for
                    // the whole body, so the craters keep their places relative to each other.
                    const float    turned = (float)std::fmod((double)p.spin * pose.time, 360.0);
                    const float    lon = p.lon + 360.0f * Hash01(seed, 4241) +
                                         360.0f * (float)r / (float)repeat + turned;
                    const OnSphere sp = Project(lat, lon, s.axisTilt);
                    if (sp.z <= 0.0f)
                        continue;  // round the back

                    const float ch = std::cos(pose.heading), sh = std::sin(pose.heading);
                    piece.pos = { pos.x + (sp.x * ch - sp.y * sh) * size,
                                  pos.y + (sp.x * sh + sp.y * ch) * size };

                    // Seen at a slant near the limb: an ellipse whose short axis points at the
                    // centre of the body and is shortened by how obliquely it is seen.
                    piece.squash = sp.z;
                    piece.angle = std::atan2(sp.y, sp.x) * RAD2DEG + pose.heading * RAD2DEG;

                    // And faded as it goes over, so it slides off the edge rather than
                    // popping. The window grows with the feature, because a large crater
                    // would otherwise stick out past the limb while it is still visible.
                    piece.brightness *= Smooth01(0.0f, std::fmax(0.12f, 2.2f * p.radius), sp.z);
                    out.push_back(piece);
                    continue;
                }

                // An orbiting part ignores `at` entirely: where it is comes from where it
                // has got to in its lap, which is the point of it.
                if (p.orbitRadius > 0.0f)
                {
                    const float period = p.orbitPeriod > 0.01f ? p.orbitPeriod : 60.0f;
                    // Every repeat and every mirror is spread evenly around the lap, so
                    // three moons are three moons rather than three moons on top of one
                    // another.
                    const float spread = (float)(r * sides + m % sides) / (float)(repeat * sides);
                    const float lap = (float)std::fmod(
                        pose.time / period + p.orbitPhase + spread + Hash01(seed, salt + 29), 1.0);
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
    std::stable_sort(out.begin(), out.end(), [](const Piece& a, const Piece& b)
                     { return a.depth < b.depth || (a.depth == b.depth && a.z < b.z); });
    return out;
}

}  // namespace Render
