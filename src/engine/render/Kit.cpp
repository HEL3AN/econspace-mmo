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
    bool  closed = false;
};

void Line(std::vector<Socket>& out, int& lines, int section, const char* type,
          const std::vector<Vector2>& at, const std::vector<float>& angle, float size, Where& w)
{
    for (size_t k = 0; k < at.size(); k++)
        out.push_back({ at[k], angle[k], type, section, lines, (int)k, size, w.source, w.copy,
                        w.local, w.spin, w.closed });
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
                        w.closed = !arc;
                        Line(out, lines, si, type, at, angle, pitch, w);
                        w.closed = false;
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

    for (size_t e = 0; e < kit.entries.size(); e++)
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

        // The lines that can take it: one half of a bilateral object (the other is its
        // mirror), the first copy of a radial one (the others repeat it).
        std::vector<int> lines;
        for (const Socket& s : sockets)
            if (s.type == type && (!bilateral || s.pos.y <= 1e-3f) && (!radial || s.copy == 0) &&
                (entry.in < 0 || s.source == entry.in) &&
                std::find(lines.begin(), lines.end(), s.line) == lines.end())
                lines.push_back(s.line);
        auto freeOn = [&](int line)
        {
            int n = 0;
            for (size_t i = 0; i < sockets.size(); i++)
                n += sockets[i].line == line && !used[i];
            return n;
        };
        // The longest free line first, then the order the sections were written in.
        // How squarely a line faces away from the object's centre, -1..1: which of two
        // equal edges is the outer one, which end of an arm is its far end.
        auto outward = [&](int line)
        {
            float sum = 0.0f;
            int   n = 0;
            for (const Socket& s : sockets)
                if (s.line == line)
                {
                    const float   len = std::hypot(s.pos.x, s.pos.y);
                    const Vector2 nrm = Turn({ 1.0f, 0.0f }, s.angle);
                    sum += len > 1e-4f ? (s.pos.x * nrm.x + s.pos.y * nrm.y) / len : 0.0f;
                    n++;
                }
            return n > 0 ? sum / (float)n : 0.0f;
        };
        const float sign = entry.prefer == "in" ? -1.0f : 1.0f;
        std::stable_sort(lines.begin(), lines.end(),
                         [&](int a, int b)
                         {
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
            if (entry.mount == "on" && s.type != "top")
                push = -reach;
            else if (entry.mount == "out")
                push = reach;
            return Vector2{ s.pos.x - centre.x + n.x * push, s.pos.y - centre.y + n.y * push };
        };

        auto place = [&](size_t i)
        {
            const Socket& s = sockets[i];
            Part          q;
            q.module = mod->id;
            q.variant = variant->id;
            q.scale = entry.scale > 0.0f ? entry.scale : s.size * 0.45f;
            q.angle = s.angle + entry.turn;
            q.at = mounted(s, q.scale, q.angle);
            q.spin = s.spin;
            q.z = entry.z;
            out.push_back(q);
            // A module covers the sockets under it, so nothing else is put on top of it.
            const float   c = std::cos(q.angle * DEG2RAD), sn = std::sin(q.angle * DEG2RAD);
            const Vector2 mid = { (box.x + 0.5f * box.width) * q.scale,
                                  (box.y + 0.5f * box.height) * q.scale };
            const Vector2 centre = { q.at.x + mid.x * c - mid.y * sn,
                                     q.at.y + mid.x * sn + mid.y * c };
            const float   span = 0.5f * q.scale * std::max(box.width, box.height);
            for (size_t j = 0; j < sockets.size(); j++)
                if (!used[j] && sockets[j].section == s.section &&
                    std::hypot(sockets[j].pos.x - centre.x, sockets[j].pos.y - centre.y) <
                        span + 0.5f * s.size)
                    used[j] = true;
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
            std::vector<size_t> all;
            for (size_t i = 0; i < sockets.size(); i++)
                if (sockets[i].line == line)
                    all.push_back(i);
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
                if (!room(sockets[i]))
                    continue;
                const int twin = offAxis ? twinOf(i) : -1;
                place(i);
                left--;
                if (radial)
                    for (size_t j = 0; j < sockets.size(); j++)
                        if (j != i && !used[j] && sockets[j].source == sockets[i].source &&
                            sockets[j].copy != 0 && sockets[j].localLine == sockets[i].localLine &&
                            sockets[j].index == sockets[i].index)
                            place(j);
                if (twin >= 0)
                {
                    // The twin is the mirror of this one: its shape reflected, not turned.
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
