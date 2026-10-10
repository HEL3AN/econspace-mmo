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

// One line of sockets along a section: an edge, an end, a centreline, a rim.
struct Where
{
    int   source, copy, local;
    float spin;
};

void Line(std::vector<Socket>& out, int& lines, int section, const char* type,
          const std::vector<Vector2>& at, const std::vector<float>& angle, float size, Where& w)
{
    for (size_t k = 0; k < at.size(); k++)
        out.push_back({ at[k], angle[k], type, section, lines, (int)k, size, w.source, w.copy,
                        w.local, w.spin });
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
}  // namespace

std::vector<Socket> Sockets(const std::vector<Part>& sections)
{
    std::vector<Socket> out;
    int                 lines = 0;
    int                 index = 0;
    for (size_t source = 0; source < sections.size(); source++)
    {
        const std::vector<Part> copies = Copies(sections[source]);
        for (size_t copy = 0; copy < copies.size(); copy++)
        {
            const Part& s = copies[copy];
            const int   si = index++;
            Where       w{ (int)source, (int)copy, 0, s.spin };
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
                    for (int end = -1; end <= 1; end += 2)
                    {
                        const Vector2 o = Turn({ 0.5f * s.length * (float)end, 0.0f }, s.angle);
                        Line(out, lines, si, "end", { { s.at.x + o.x, s.at.y + o.y } },
                             { s.angle + (end > 0 ? 0.0f : 180.0f) }, std::fmin(s.width, pitch), w);
                    }
                    break;
                }
                case Form::Chevron:
                {
                    // A wedge: its tail is an end.
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
                    // A polygon's rim is its outline, not the circle through its corners:
                    // a socket on a flat face sits on the face.
                    auto outline = [&](float t, float radius)
                    {
                        if (s.form != Form::Polygon || s.sides < 3)
                            return radius;
                        const float sector = 360.0f / (float)s.sides;
                        float       local = std::fmod(t - s.angle, sector);
                        if (local < 0.0f)
                            local += sector;
                        return radius * std::cos(PI / (float)s.sides) /
                               std::cos((local - 0.5f * sector) * DEG2RAD);
                    };
                    // ...and faces the way its face does.
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
                            const Vector2 o = Turn({ outline(t, radius), 0.0f }, t);
                            at.push_back({ s.at.x + o.x, s.at.y + o.y });
                            angle.push_back(normal(t));
                        }
                        Line(out, lines, si, type, at, angle, pitch, w);
                    };
                    const bool solid = s.form == Form::Disc || s.form == Form::Polygon;
                    rim(s.radius, solid ? "edge" : "ring");
                    if (solid && s.radius >= 0.2f)
                        rim(s.radius * 0.55f, "top");
                    break;
                }
                default: break;
            }
        }
    }
    return out;
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
    auto       twinOf = [&](size_t i) -> int
    {
        // Across the object's own axis: same kind of place, y negated, facing the mirror way.
        const Socket& s = sockets[i];
        for (size_t j = 0; j < sockets.size(); j++)
            if (j != i && !used[j] && sockets[j].type == s.type &&
                std::fabs(sockets[j].pos.x - s.pos.x) < 1e-3f &&
                std::fabs(sockets[j].pos.y + s.pos.y) < 1e-3f)
                return (int)j;
        return -1;
    };

    for (size_t e = 0; e < kit.entries.size(); e++)
    {
        const KitEntry& entry = kit.entries[e];
        const int       salt = 7001 + (int)e * 13;

        // Which module: by id, or one of those carrying the tag, chosen once for the entry.
        std::vector<const Module*> candidates;
        for (const Module& m : Modules::All())
        {
            const bool tagged = std::find(m.tags.begin(), m.tags.end(), entry.of) != m.tags.end();
            if (entry.byTag ? tagged : m.id == entry.of)
                candidates.push_back(&m);
        }
        if (candidates.empty())
            continue;
        const Module* mod = candidates[std::min(
            candidates.size() - 1, (size_t)(Hash01(seed, salt) * (float)candidates.size()))];
        if (mod->variants.empty())
            continue;
        // One variant for every copy the entry places: a row of the same hatch is a row,
        // a row of different hatches is clutter.
        std::string variant = entry.variant;
        if (variant.empty())
            variant = mod->variants[std::min(mod->variants.size() - 1,
                                             (size_t)(Hash01(seed, salt + 2) *
                                                      (float)mod->variants.size()))]
                          .id;
        const std::string type =
            !entry.on.empty() ? entry.on : (mod->sockets.empty() ? "edge" : mod->sockets[0]);

        // How many, with every count in the range equally likely.
        const float u = Hash01(seed, salt + 4);
        int         want = (int)std::floor(entry.lo + (entry.hi - entry.lo + 1.0f) * u);
        want = std::max((int)entry.lo, std::min((int)entry.hi, want));
        if (want <= 0)
            continue;

        // The lines that can take it, in an order the seed decides. Bilateral objects place
        // from one half (and the axis) and mirror into the other.
        std::vector<int> lines;
        for (const Socket& s : sockets)
            if (s.type == type && (!bilateral || s.pos.y <= 1e-3f) && (!radial || s.copy == 0) &&
                (entry.in < 0 || s.source == entry.in) &&
                std::find(lines.begin(), lines.end(), s.line) == lines.end())
                lines.push_back(s.line);
        std::sort(lines.begin(), lines.end(), [&](int a, int b)
                  { return Hash01(seed, salt + 50 + a) < Hash01(seed, salt + 50 + b); });

        auto place = [&](size_t i)
        {
            const Socket& s = sockets[i];
            Part          q;
            q.module = mod->id;
            q.variant = variant;
            q.scale = entry.scale > 0.0f ? entry.scale : s.size * 0.45f;
            const Vector2 in = Turn({ -entry.inset * q.scale, 0.0f }, s.angle);
            q.at = { s.pos.x + in.x, s.pos.y + in.y };
            q.angle = s.angle + entry.turn;
            q.spin = s.spin;
            out.push_back(q);
            used[i] = true;
            taken[(size_t)s.section]++;
        };

        int left = want;
        for (int line : lines)
        {
            if (left <= 0)
                break;
            std::vector<size_t> free;
            for (size_t i = 0; i < sockets.size(); i++)
                if (sockets[i].line == line && !used[i] && room(sockets[i]))
                    free.push_back(i);
            if (free.empty())
                continue;
            const bool offAxis = bilateral && sockets[free[0]].pos.y < -1e-3f;
            const int  n = std::min(offAxis ? (left + 1) / 2 : left, (int)free.size());
            for (int k = 0; k < n; k++)
            {
                // Evenly spread along the line: spacing is what reads as designed.
                const size_t i = free[std::min(
                    free.size() - 1, (size_t)(((float)k + 0.5f) * (float)free.size() / (float)n))];
                if (used[i] || !room(sockets[i]))
                    continue;
                const int twin = offAxis ? twinOf(i) : -1;
                place(i);
                left--;
                if (radial)
                    // The same place on every other copy of the section: three arms, three
                    // hatches, where a single one would read as an accident.
                    for (size_t j = 0; j < sockets.size(); j++)
                        if (j != i && !used[j] && sockets[j].source == sockets[i].source &&
                            sockets[j].copy != 0 && sockets[j].localLine == sockets[i].localLine &&
                            sockets[j].index == sockets[i].index)
                            place(j);
                if (twin >= 0)
                {
                    // The twin is placed at its own socket by mirroring this one.
                    used[(size_t)twin] = true;
                    taken[(size_t)sockets[(size_t)twin].section]++;
                    Part q = out.back();
                    q.mirror = true;
                    q.mirrorOnly = true;
                    out.push_back(q);
                    left--;
                }
            }
        }
    }
    return out;
}
}  // namespace Render
