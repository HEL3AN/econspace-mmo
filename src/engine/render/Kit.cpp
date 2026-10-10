#include "render/Modules.h"
#include "render/Silhouette.h"

#include <algorithm>
#include <cmath>

// Sections, sockets and kits (#240 phase 3): where an object's modules go, decided from its
// silhouette rather than written part by part. See Silhouette.h for the data and the rules.
namespace Render
{
namespace
{
float Hash01(int seed, int salt)
{
    // The same mixing the composer uses for jitter: an integer hash, no floating state, so
    // every client places the same hatch in the same socket.
    unsigned h = (unsigned)seed * 0x9E3779B1u ^ (unsigned)salt * 0x85EBCA77u;
    h ^= h >> 15;
    h *= 0x2C1B3C6Du;
    h ^= h >> 12;
    h *= 0x297A2D39u;
    h ^= h >> 15;
    return (float)(h & 0xFFFFFFu) / (float)0x1000000u;
}

Vector2 Turn(Vector2 v, float degrees)
{
    const float c = std::cos(degrees * DEG2RAD), s = std::sin(degrees * DEG2RAD);
    return { v.x * c - v.y * s, v.x * s + v.y * c };
}

// One line of sockets along a section: an edge, an end, a centreline, a rim, its middle.
struct Where
{
    int   source, copy, local;
    float spin;
    bool  closed = false;
    bool  axial = false;
    float axis = 0.0f;
};

void Line(std::vector<Socket>& out, int& lines, int section, const char* type,
          const std::vector<Vector2>& at, const std::vector<float>& angle, float size, Where& w)
{
    for (size_t k = 0; k < at.size(); k++)
        out.push_back({ at[k], angle[k], type, section, lines, (int)k, size, w.source, w.copy,
                        w.local, w.spin, w.closed, w.axial, w.axis });
    lines++;
    w.local++;
}

// The places along a straight run of length `length`, `pitch` apart, centred.
std::vector<float> Along(float length, float pitch)
{
    const int          n = std::max(1, (int)std::floor(length * 0.9f / pitch));
    std::vector<float> xs;
    for (int k = 0; k < n; k++)
        xs.push_back(((float)k - 0.5f * (float)(n - 1)) * pitch);
    return xs;
}

// Every placed copy of a section: a section may repeat about the centre or mirror across
// the axis like any part, and each copy has sockets of its own.
std::vector<Part> Copies(const Part& s)
{
    std::vector<Part> out;
    const int         repeat = std::max(1, s.repeat);
    for (int r = 0; r < repeat; r++)
        for (int m = 0; m < (s.mirror ? 2 : 1); m++)
        {
            Part        c = s;
            const float flip = m == 0 ? 1.0f : -1.0f;
            const float rot = 360.0f / (float)repeat * (float)r;
            c.at = Turn({ s.at.x, s.at.y * flip }, rot);
            c.angle = s.angle * flip + rot;
            c.repeat = 1;
            c.mirror = false;
            out.push_back(c);
        }
    return out;
}

// How far a polygon's outline is from its centre in direction `t` (degrees, the polygon's
// own frame): a socket on a flat face sits on the face, not on the circle through the corners.
float PolygonReach(const Part& s, float t, float radius)
{
    if (s.form != Form::Polygon || s.sides < 3)
        return radius;
    const float sector = 360.0f / (float)s.sides;
    float       local = std::fmod(t, sector);
    if (local < 0.0f)
        local += sector;
    return radius * std::cos(PI / (float)s.sides) / std::cos((local - 0.5f * sector) * DEG2RAD);
}

}  // namespace

// Whether a placed section covers a point, by more than a sliver: a socket exactly on the
// line where two sections meet is still a place on the one it belongs to.
bool Covers(const Part& s, Vector2 p)
{
    const float   eps = 0.01f;
    const Vector2 d = Turn({ p.x - s.at.x, p.y - s.at.y }, -s.angle);
    const float   r = std::hypot(d.x, d.y);
    switch (s.form)
    {
        case Form::Disc: return r < s.radius - eps;
        case Form::Ring: return r < s.radius - eps && r > s.radius - s.width + eps;
        case Form::Arc:
        {
            if (r >= s.radius - eps || r <= s.radius - s.width + eps)
                return false;
            const float from = std::fmin(s.arcFrom, s.arcTo);
            const float span = std::fabs(s.arcTo - s.arcFrom);
            float       a = std::fmod(std::atan2(d.y, d.x) * RAD2DEG - from, 360.0f);
            if (a < 0.0f)
                a += 360.0f;
            return a < span;
        }
        case Form::Polygon:
        {
            const float t = std::atan2(d.y, d.x) * RAD2DEG;
            if (r >= PolygonReach(s, t, s.radius) - eps)
                return false;
            return s.filled || r > PolygonReach(s, t, s.radius - s.width) + eps;
        }
        case Form::Capsule:
        {
            const float x = std::fmax(-0.5f * s.length, std::fmin(0.5f * s.length, d.x));
            return std::hypot(d.x - x, d.y) < 0.5f * s.width - eps;
        }
        case Form::Bar:
        case Form::Lattice:
            return std::fabs(d.x) < 0.5f * s.length - eps && std::fabs(d.y) < 0.5f * s.width - eps;
        case Form::Chevron:
        {
            // The base at the tail (-x), narrowing to `tip` of it at the point (+x).
            if (std::fabs(d.x) >= 0.5f * s.length - eps)
                return false;
            const float u = (d.x + 0.5f * s.length) / s.length;
            return std::fabs(d.y) < 0.5f * s.width * (1.0f - u + s.tip * u) - eps;
        }
        default: return false;
    }
}

std::vector<Socket> Sockets(const std::vector<Part>& sections)
{
    std::vector<Socket> out;
    int                 lines = 0;
    int                 index = 0;
    // Every placed copy, in the order they are drawn: by `z`, then as written.
    std::vector<Part> placed;
    std::vector<int>  order;
    for (const Part& section : sections)
        for (const Part& c : Copies(section))
            placed.push_back(c);
    for (size_t i = 0; i < placed.size(); i++)
        order.push_back((int)i);
    std::stable_sort(order.begin(), order.end(),
                     [&](int a, int b) { return placed[(size_t)a].z < placed[(size_t)b].z; });
    std::vector<int> rank(placed.size());
    for (size_t k = 0; k < order.size(); k++)
        rank[(size_t)order[k]] = (int)k;

    for (size_t source = 0; source < sections.size(); source++)
    {
        const std::vector<Part> copies = Copies(sections[source]);
        for (size_t copy = 0; copy < copies.size(); copy++)
        {
            const Part& s = copies[copy];
            const int   si = index++;
            Where       w{ (int)source, (int)copy, 0, s.spin };
            w.axial = s.form == Form::Bar || s.form == Form::Capsule || s.form == Form::Lattice ||
                      s.form == Form::Chevron;
            w.axis = s.angle;
            switch (s.form)
            {
                case Form::Bar:
                case Form::Capsule:
                case Form::Lattice:
                {
                    // Two long edges, two ends and a centreline: a hull block's places.
                    const float pitch = s.pitch > 0.0f ? s.pitch : std::fmax(s.width, 0.08f);
                    const std::vector<float> xs = Along(s.length, pitch);
                    for (int side = -1; side <= 1; side += 2)
                    {
                        std::vector<Vector2> at;
                        std::vector<float>   angle;
                        for (float x : xs)
                        {
                            const Vector2 o = Turn({ x, 0.5f * s.width * (float)side }, s.angle);
                            at.push_back({ s.at.x + o.x, s.at.y + o.y });
                            angle.push_back(s.angle + 90.0f * (float)side);
                        }
                        Line(out, lines, si, "edge", at, angle, pitch, w);
                    }
                    {
                        std::vector<Vector2> at;
                        std::vector<float>   angle;
                        for (float x : xs)
                        {
                            const Vector2 o = Turn({ x, 0.0f }, s.angle);
                            at.push_back({ s.at.x + o.x, s.at.y + o.y });
                            angle.push_back(s.angle);
                        }
                        Line(out, lines, si, "top", at, angle, pitch, w);
                    }
                    // An end is where the drawing ends: a capsule's round cap reaches half
                    // its width past `length`, and a module mounted at the cap's base would
                    // sink into it.
                    const float reach =
                        0.5f * s.length + (s.form == Form::Capsule ? 0.5f * s.width : 0.0f);
                    for (int end = -1; end <= 1; end += 2)
                    {
                        const Vector2 o = Turn({ reach * (float)end, 0.0f }, s.angle);
                        Line(out, lines, si, "end", { { s.at.x + o.x, s.at.y + o.y } },
                             { s.angle + (end > 0 ? 0.0f : 180.0f) }, std::fmin(s.width, pitch), w);
                    }
                    break;
                }
                case Form::Chevron:
                {
                    // A wedge: its two slanted sides are edges, facing square out of each
                    // side as a polygon's faces do, and its tail is an end.
                    const float pitch = s.pitch > 0.0f ? s.pitch : std::fmax(s.width, 0.08f);
                    const float hb = 0.5f * s.width, ht = 0.5f * s.width * s.tip;
                    const float run = std::hypot(s.length, hb - ht);
                    const std::vector<float> xs = Along(run, pitch);
                    for (int side = -1; side <= 1; side += 2)
                    {
                        // The side's middle, its direction from tail to tip, and the normal.
                        const Vector2 mid = { 0.0f, 0.5f * (hb + ht) * (float)side };
                        const Vector2 dir = { s.length / run, (ht - hb) * (float)side / run };
                        const float   face =
                            s.angle + std::atan2((float)side * s.length, hb - ht) * RAD2DEG;
                        std::vector<Vector2> at;
                        std::vector<float>   angle;
                        for (float x : xs)
                        {
                            const Vector2 o =
                                Turn({ mid.x + dir.x * x, mid.y + dir.y * x }, s.angle);
                            at.push_back({ s.at.x + o.x, s.at.y + o.y });
                            angle.push_back(face);
                        }
                        Line(out, lines, si, "edge", at, angle, pitch, w);
                    }
                    const Vector2 o = Turn({ -0.5f * s.length, 0.0f }, s.angle);
                    Line(out, lines, si, "end", { { s.at.x + o.x, s.at.y + o.y } },
                         { s.angle + 180.0f }, s.width * 0.6f, w);
                    break;
                }
                case Form::Disc:
                case Form::Polygon:
                case Form::Ring:
                case Form::Arc:
                {
                    // A rim of sockets facing out, and for a solid body an inner ring of tops.
                    const float pitch =
                        s.pitch > 0.0f ? s.pitch : std::fmax(s.radius * 0.35f, 0.08f);
                    const bool  arc = s.form == Form::Arc;
                    const float from = arc ? s.arcFrom : 0.0f;
                    const float span = arc ? s.arcTo - s.arcFrom : 360.0f;
                    // A polygon's rim is its outline, and a socket there faces the way its
                    // face does.
                    auto normal = [&](float t)
                    {
                        if (s.form != Form::Polygon || s.sides < 3)
                            return t;
                        const float sector = 360.0f / (float)s.sides;
                        return s.angle + (std::floor((t - s.angle) / sector) + 0.5f) * sector;
                    };
                    auto rim = [&](float radius, const char* type)
                    {
                        const int n =
                            std::max(3, (int)std::lround(2.0f * PI * radius * std::fabs(span) /
                                                         360.0f / pitch));
                        std::vector<Vector2> at;
                        std::vector<float>   angle;
                        for (int k = 0; k < n; k++)
                        {
                            const float t = s.angle + from +
                                            span * (arc ? ((float)k + 0.5f) : (float)k) / (float)n;
                            const Vector2 o =
                                Turn({ PolygonReach(s, t - s.angle, radius), 0.0f }, t);
                            at.push_back({ s.at.x + o.x, s.at.y + o.y });
                            angle.push_back(normal(t));
                        }
                        w.closed = !arc;
                        Line(out, lines, si, type, at, angle, pitch, w);
                        w.closed = false;
                    };
                    const bool solid = s.form == Form::Disc || s.form == Form::Polygon;
                    // A ring's sockets are on the middle of its band, which is drawn from
                    // `radius - width` out to `radius`: on its outer edge a module looked
                    // pushed off the band it belongs to.
                    rim(solid ? s.radius : s.radius - 0.5f * s.width, solid ? "edge" : "ring");
                    if (solid && s.radius >= 0.2f)
                        rim(s.radius * 0.55f, "top");
                    break;
                }
                default: break;
            }
            // The section's own centre, facing the section's own way: the one place for the
            // thing a section is built round -- a hub's tower, a hull's bridge, the beacon in
            // a ring. An arc's centre is the middle of its band, where the arc actually is.
            float   middle = 0.0f;
            Vector2 centre = s.at;
            float   facing = s.angle;
            switch (s.form)
            {
                case Form::Bar:
                case Form::Capsule:
                case Form::Lattice:
                case Form::Chevron: middle = std::fmin(s.width, s.length); break;
                case Form::Disc:
                case Form::Polygon: middle = s.radius; break;
                case Form::Ring: middle = s.radius - s.width; break;
                case Form::Arc:
                {
                    middle = s.width;
                    facing = s.angle + 0.5f * (s.arcFrom + s.arcTo);
                    const Vector2 o = Turn({ s.radius - 0.5f * s.width, 0.0f }, facing);
                    centre = { s.at.x + o.x, s.at.y + o.y };
                    break;
                }
                default: break;
            }
            if (middle > 0.0f)
                Line(out, lines, si, "middle", { centre }, { facing }, middle, w);
        }
    }

    // A socket under another section is no place at all (#240): covered by one drawn over
    // it, a module there is hidden or pokes out from under it; lying on one drawn beneath
    // it, the module hangs over a neighbour -- unless its own section sits on that one (a
    // panel on a hull, a ring round a hub), which is what a section on a section is for.
    // Only the topmost section at the point decides: what is under that is not seen.
    std::vector<Socket> open;
    for (const Socket& k : out)
    {
        int top = -1;
        for (size_t b = 0; b < placed.size(); b++)
            if ((int)b != k.section && Covers(placed[b], k.pos) &&
                (top < 0 || rank[b] > rank[(size_t)top]))
                top = (int)b;
        if (top >= 0 && (rank[(size_t)top] > rank[(size_t)k.section] ||
                         !Covers(placed[(size_t)top], placed[(size_t)k.section].at)))
            continue;
        open.push_back(k);
    }
    return open;
}

bool IsSocketKind(const std::string& kind)
{
    for (const char* k : { "edge", "end", "top", "ring", "middle", "bow", "stern", "front", "side",
                           "spine", "bottom" })
        if (kind == k)
            return true;
    return false;
}

bool Offers(const std::vector<Socket>& sockets, size_t i, const std::string& kind)
{
    const Socket& s = sockets[i];
    if (kind == s.type)
        return true;
    // Within 45 degrees of +x or of -x: forward, aft.
    const float along = std::cos(s.angle * DEG2RAD);
    if (kind == "bow" || kind == "stern")
        return s.type == "end" && (kind == "bow" ? along > 0.7071f : along < -0.7071f);
    if (kind == "bottom")
        return s.type == "top";
    if (!s.axial)
        return false;
    if (kind == "spine")
        return s.type == "top";
    // The rest are one place on a line: which one is read off the line's other sockets.
    if (kind == "front")
    {
        if (s.type != "top" || std::fabs(std::cos(s.axis * DEG2RAD)) < 0.7071f)
            return false;
        for (const Socket& o : sockets)
            if (o.line == s.line && o.pos.x > s.pos.x + 1e-4f)
                return false;
        return true;
    }
    if (kind == "side")
    {
        // The middle of what is open of the flank: where a section beneath covers the aft
        // half of it, the side moves forward to the half that is there.
        if (s.type != "edge")
            return false;
        std::vector<int> open;
        for (const Socket& o : sockets)
            if (o.line == s.line)
                open.push_back(o.index);
        std::sort(open.begin(), open.end());
        return s.index == open[(open.size() - 1) / 2];
    }
    return false;
}

std::vector<Part> PlaceKit(const Kit& kit, const std::vector<Part>& sections, int seed)
{
    std::vector<Part> out;
    if (kit.entries.empty() || sections.empty())
        return out;
    const std::vector<Socket> sockets = Sockets(sections);
    std::vector<bool>         used(sockets.size(), false);

    // The rule against mush: a section keeps at least `plain` of its sockets empty.
    std::vector<int> total, taken;
    for (const Socket& s : sockets)
    {
        if (s.section >= (int)total.size())
        {
            total.resize((size_t)s.section + 1, 0);
            taken.resize((size_t)s.section + 1, 0);
        }
        total[(size_t)s.section]++;
    }
    auto room = [&](const Socket& s)
    {
        const int cap = std::max(
            1, (int)std::floor((float)total[(size_t)s.section] * (1.0f - kit.plain) + 0.001f));
        return taken[(size_t)s.section] < cap;
    };
    const bool bilateral = kit.symmetry == "bilateral";
    const bool radial = kit.symmetry == "radial";

    // Function before looks (#279): the lines that decide what the object can do are placed
    // first, so a drive or a hold never loses its place to trim. Each line keeps its own
    // salt, so the order changes nothing about what the seed chooses.
    std::vector<size_t> sequence;
    for (int pass = 0; pass < 2; pass++)
        for (size_t e = 0; e < kit.entries.size(); e++)
            if (kit.entries[e].fit == (pass == 0))
                sequence.push_back(e);

    for (size_t e : sequence)
    {
        const KitEntry& entry = kit.entries[e];
        const int       salt = 7001 + (int)e * 13;

        // Which module: by id, or one of those carrying the tag, chosen once for the line.
        std::vector<const Module*> candidates;
        for (const Module& m : Modules::All())
        {
            const bool tagged = std::find(m.tags.begin(), m.tags.end(), entry.of) != m.tags.end();
            // A tagged module with none of the variants the line allows is not a candidate.
            if ((entry.byTag ? tagged : m.id == entry.of) &&
                !AllowedVariants(m, entry.variants, entry.except).empty())
                candidates.push_back(&m);
        }
        if (candidates.empty())
            continue;
        const Module* mod = candidates[std::min(
            candidates.size() - 1, (size_t)(Hash01(seed, salt) * (float)candidates.size()))];
        // One variant for every copy the line places: a row of the same hatch is a row, a
        // row of different hatches is clutter.
        const std::vector<const ModuleVariant*> allowed =
            AllowedVariants(*mod, entry.variants, entry.except);
        const ModuleVariant* variant = nullptr;
        for (const ModuleVariant* v : allowed)
            if (v->id == entry.variant)
                variant = v;
        if (variant == nullptr)
            variant = allowed[std::min(allowed.size() - 1,
                                       (size_t)(Hash01(seed, salt + 2) * (float)allowed.size()))];
        const std::string type =
            !entry.on.empty() ? entry.on : (mod->sockets.empty() ? "edge" : mod->sockets[0]);

        // How many, every count in the range equally likely. The seed decides this and the
        // variant; where they go is decided by rules, because a seed choosing places is what
        // made the first kits look scattered rather than designed.
        const float u = Hash01(seed, salt + 4);
        int         want = (int)std::floor(entry.lo + (entry.hi - entry.lo + 1.0f) * u);
        want = std::max((int)entry.lo, std::min((int)entry.hi, want));
        if (want <= 0)
            continue;

        // The sockets of that kind, and the lines that can take it: one half of a bilateral
        // object (the other is its mirror), the first copy of a radial one (the others repeat
        // it).
        std::vector<char> fits(sockets.size(), 0);
        for (size_t i = 0; i < sockets.size(); i++)
            fits[i] = Offers(sockets, i, type);
        std::vector<int> lines;
        for (size_t i = 0; i < sockets.size(); i++)
        {
            const Socket& s = sockets[i];
            if (fits[i] && (!bilateral || s.pos.y <= 1e-3f) && (!radial || s.copy == 0) &&
                (entry.in.empty() ||
                 std::find(entry.in.begin(), entry.in.end(), s.source) != entry.in.end()) &&
                std::find(lines.begin(), lines.end(), s.line) == lines.end())
                lines.push_back(s.line);
        }
        auto freeOn = [&](int line)
        {
            int n = 0;
            for (size_t i = 0; i < sockets.size(); i++)
                n += fits[i] && sockets[i].line == line && !used[i];
            return n;
        };
        // The longest free line first, then the order the sections were written in.
        // How squarely a line faces away from the object's centre, -1..1: which of two
        // equal edges is the outer one, which end of an arm is its far end.
        auto outward = [&](int line)
        {
            float sum = 0.0f;
            int   n = 0;
            for (size_t i = 0; i < sockets.size(); i++)
                if (fits[i] && sockets[i].line == line)
                {
                    const Socket& s = sockets[i];
                    const float   len = std::hypot(s.pos.x, s.pos.y);
                    const Vector2 nrm = Turn({ 1.0f, 0.0f }, s.angle);
                    sum += len > 1e-4f ? (s.pos.x * nrm.x + s.pos.y * nrm.y) / len : 0.0f;
                    n++;
                }
            return n > 0 ? sum / (float)n : 0.0f;
        };
        const float sign = entry.prefer == "in" ? -1.0f : 1.0f;
        // On a bilateral object an even count is pairs (#279): two engines are the pair of
        // nacelles, not one on the axis and a pair beside it.
        auto paired = [&](int line)
        {
            for (size_t i = 0; i < sockets.size(); i++)
                if (sockets[i].line == line && sockets[i].pos.y < -1e-3f)
                    return true;
            return false;
        };
        const bool pairsFirst = bilateral && want % 2 == 0;
        std::stable_sort(lines.begin(), lines.end(),
                         [&](int a, int b)
                         {
                             if (pairsFirst && paired(a) != paired(b))
                                 return paired(a);
                             const int fa = freeOn(a), fb = freeOn(b);
                             if (fa != fb)
                                 return fa > fb;
                             return sign * outward(a) > sign * outward(b) + 1e-3f;
                         });

        const Rectangle& box = variant->bounds;

        // Where the module's origin goes for a socket: placed by its box, so that its edge
        // meets the hull's edge however the drawing sits about its own origin.
        auto mounted = [&](const Socket& s, float sc, float angle)
        {
            const float   c = std::cos(angle * DEG2RAD), sn = std::sin(angle * DEG2RAD);
            const Vector2 mid = { (box.x + 0.5f * box.width) * sc,
                                  (box.y + 0.5f * box.height) * sc };
            const Vector2 centre = { mid.x * c - mid.y * sn, mid.x * sn + mid.y * c };
            const Vector2 n = Turn({ 1.0f, 0.0f }, s.angle);
            // How far the box reaches from its middle along the outward direction.
            const float ax = std::fabs(c * n.x + sn * n.y), ay = std::fabs(-sn * n.x + c * n.y);
            const float reach = 0.5f * sc * (box.width * ax + box.height * ay);
            float       push = 0.0f;  // "centre", and any socket inside the hull
            if (entry.mount == "on" && s.type != "top" && s.type != "middle")
                push = -reach;
            else if (entry.mount == "out")
                push = reach;
            return Vector2{ s.pos.x - centre.x + n.x * push, s.pos.y - centre.y + n.y * push };
        };

        // A handed module (#279) is drawn as the one on the +y side -- a wing, root at the hull
        // and tip outward -- and its pair is that drawing reflected. On a bilateral object it
        // is placed as the +y one and drawn mirrored, one part for both, so the pair shares
        // every roll: a left wing swept at 110 degrees and a right one at 120 is not a pair.
        const bool handed = mod->handed && bilateral;

        auto place = [&](size_t i, bool pair)
        {
            const Socket& s = sockets[i];
            Part          q;
            q.module = mod->id;
            q.variant = variant->id;
            q.scale = entry.scale > 0.0f ? entry.scale : s.size * 0.45f;
            const bool reflect = handed && s.pos.y < -1e-3f;
            // The socket the drawing is placed at: the +y twin of this one, for a handed module.
            Socket at = s;
            if (reflect)
            {
                at.pos.y = -s.pos.y;
                at.angle = -s.angle;
            }
            // A handed module's turn is measured from its drawing as it stands on the +y side
            // facing out: no turn is the wing as drawn.
            q.angle = at.angle + entry.turn - (reflect ? 90.0f : 0.0f);
            q.at = mounted(at, q.scale, q.angle);
            q.spin = s.spin;
            q.z = entry.z;
            if (reflect)
            {
                q.mirror = true;
                q.mirrorOnly = !pair;  // without its twin, only the -y one is drawn
            }
            out.push_back(q);
            // A module covers the sockets under it, so nothing else is put on top of it -- by
            // its footprint, its box with the corners rounded off, not a circle round it: a
            // pod laid along a keel covers the keel, not the flanks beside it.
            const float   flip = reflect ? -1.0f : 1.0f;
            const float   c = std::cos(q.angle * DEG2RAD), sn = std::sin(q.angle * DEG2RAD);
            const Vector2 mid = { (box.x + 0.5f * box.width) * q.scale,
                                  (box.y + 0.5f * box.height) * q.scale };
            const Vector2 centre = { q.at.x + mid.x * c - mid.y * sn,
                                     (q.at.y + mid.x * sn + mid.y * c) * flip };
            const float   span = 0.5f * q.scale * std::max(box.width, box.height);
            const float   hx = 0.5f * q.scale * box.width + 0.5f * s.size;
            const float   hy = 0.5f * q.scale * box.height + 0.5f * s.size;
            for (size_t j = 0; j < sockets.size(); j++)
            {
                if (used[j] || sockets[j].section != s.section)
                    continue;
                const Vector2 d = { sockets[j].pos.x - centre.x, sockets[j].pos.y - centre.y };
                // In the module's own frame, which on the reflected side is reflected too.
                const Vector2 local = Turn({ d.x, d.y * flip }, -q.angle);
                if (std::hypot(d.x, d.y) < span + 0.5f * s.size && std::fabs(local.x) < hx &&
                    std::fabs(local.y) < hy)
                    used[j] = true;
            }
            used[i] = true;
            taken[(size_t)s.section]++;
        };
        auto twinOf = [&](size_t i) -> int
        {
            const Socket& s = sockets[i];
            for (size_t j = 0; j < sockets.size(); j++)
                if (j != i && !used[j] && sockets[j].type == s.type &&
                    std::fabs(sockets[j].pos.x - s.pos.x) < 1e-3f &&
                    std::fabs(sockets[j].pos.y + s.pos.y) < 1e-3f)
                    return (int)j;
            return -1;
        };

        int left = want;
        for (int line : lines)
        {
            if (left <= 0)
                break;
            // On a bilateral object a straight line across the axis -- the face of a crossbar
            // -- is two lines meeting there: its -y half is placed and the other is the
            // mirror. The place on the axis belongs to neither: it takes the odd one of an odd
            // count, and nothing else, or a pair would come out odd.
            std::vector<size_t> all;
            const bool          half = bilateral && paired(line);
            if (half && left % 2 == 1)
                for (size_t i = 0; i < sockets.size(); i++)
                    if (fits[i] && sockets[i].line == line && !used[i] && !sockets[i].closed &&
                        std::fabs(sockets[i].pos.y) <= 1e-3f && (entry.fit || room(sockets[i])))
                    {
                        place(i, false);
                        left--;
                        break;
                    }
            if (left <= 0)
                break;
            for (size_t i = 0; i < sockets.size(); i++)
                if (fits[i] && sockets[i].line == line &&
                    (!half || sockets[i].closed || sockets[i].pos.y < -1e-3f))
                    all.push_back(i);
            if (all.empty())
                continue;
            const bool offAxis = bilateral && sockets[all[0]].pos.y < -1e-3f;
            const int  n = std::min(offAxis ? (left + 1) / 2 : left, (int)all.size());
            const int  m = (int)all.size();
            // Where on the line each of the n goes. A straight line: spread over it and
            // symmetric about its middle. A closed rim: spread about the socket that faces
            // furthest out from the object, half a rim at most, so a pod's dock is on its
            // far side and not facing the hub it hangs from.
            std::vector<size_t> picks;
            if (sockets[all[0]].closed)
            {
                int   best = 0;
                float bestOut = -2.0f;
                for (int k = 0; k < m; k++)
                {
                    const Socket& s = sockets[all[(size_t)k]];
                    const float   len = std::hypot(s.pos.x, s.pos.y);
                    const Vector2 nrm = Turn({ 1.0f, 0.0f }, s.angle);
                    const float o = len > 1e-4f ? (s.pos.x * nrm.x + s.pos.y * nrm.y) / len : 0.0f;
                    if (o > bestOut + 1e-4f)
                    {
                        bestOut = o;
                        best = k;
                    }
                }
                const float step = std::max(1.0f, (float)m / (2.0f * (float)n));
                for (int k = 0; k < n; k++)
                {
                    const int at =
                        best + (int)std::lround(((float)k - 0.5f * (float)(n - 1)) * step);
                    picks.push_back(all[(size_t)(((at % m) + m) % m)]);
                }
            }
            else
                for (int k = 0; k < n; k++)
                    picks.push_back(all[std::min(
                        all.size() - 1, (size_t)(((float)k + 0.5f) * (float)m / (float)n))]);
            // Spread over the whole line and symmetric about its middle: a pair sits a
            // quarter of the way in from each end, not wherever free sockets happen to be.
            for (size_t k = 0; k < picks.size() && left > 0; k++)
            {
                size_t i = picks[k];
                if (used[i])
                {
                    // Covered by something placed before: the nearest free socket on the
                    // same line rather than nothing -- a crane hidden behind a radiator
                    // used to vanish without a word.
                    size_t best = all.size();
                    int    bestD = 1 << 30;
                    size_t at = 0;
                    for (size_t q = 0; q < all.size(); q++)
                        if (all[q] == i)
                            at = q;
                    for (size_t q = 0; q < all.size(); q++)
                        if (!used[all[q]] && std::abs((int)q - (int)at) < bestD)
                        {
                            bestD = std::abs((int)q - (int)at);
                            best = q;
                        }
                    if (best == all.size())
                        continue;
                    i = all[best];
                }
                if (!entry.fit && !room(sockets[i]))
                    continue;
                const int twin = offAxis && sockets[i].pos.y < -1e-3f ? twinOf(i) : -1;
                place(i, twin >= 0);
                left--;
                if (radial)
                    for (size_t j = 0; j < sockets.size(); j++)
                        if (j != i && !used[j] && sockets[j].source == sockets[i].source &&
                            sockets[j].copy != 0 && sockets[j].localLine == sockets[i].localLine &&
                            sockets[j].index == sockets[i].index)
                            place(j, false);
                if (twin >= 0)
                {
                    // The twin is the mirror of this one: its shape reflected, not turned, and
                    // drawn from the same part, so the two share every roll -- a pod of three
                    // tanks is not mirrored by a pod of two.
                    used[(size_t)twin] = true;
                    taken[(size_t)sockets[(size_t)twin].section]++;
                    out.back().mirror = true;
                    out.back().mirrorOnly = false;
                    left--;
                }
            }
        }
    }
    return out;
}
}  // namespace Render
