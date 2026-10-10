#include "render/Modules.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <raylib.h>
#include <nlohmann/json.hpp>

namespace Render
{
namespace
{
using json = nlohmann::json;
std::vector<Module> g_modules;

// The box a variant covers in its own unit: composed as the game would draw it -- rows,
// rings and turns laid out, ranges at one seed -- and measured from the pieces, so a
// drawing that is all on one side of its origin is mounted by where it actually is.
//
// Each piece is measured by what it draws, not by a circle around it: a circle, ring or
// polygon by its radius; an arc by the part of the annulus its span covers (its two ends,
// inner and outer, and any of the four axis points of the outer edge it passes); a bar or a
// lattice by the corners of its turned rectangle, a chevron by its own corners, a capsule by
// its two end caps. A circle around a long bar is as wide as the bar is long, and a quarter
// arc measured as its whole circle is twice its size each way; either pushed a module
// standing `out` off the hull it was meant to stand on.
void Include(float lo[2], float hi[2], Vector2 p, float r = 0.0f)
{
    lo[0] = std::min(lo[0], p.x - r);
    lo[1] = std::min(lo[1], p.y - r);
    hi[0] = std::max(hi[0], p.x + r);
    hi[1] = std::max(hi[1], p.y + r);
}

void Measure(const Piece& p, float lo[2], float hi[2])
{
    const float   a = p.angle * DEG2RAD;
    const Vector2 along = { std::cos(a), std::sin(a) };
    const Vector2 across = { -along.y, along.x };
    // A point `u` along the piece's axis and `v` across it.
    auto at = [&](float u, float v)
    {
        return Vector2{ p.pos.x + along.x * u + across.x * v,
                        p.pos.y + along.y * u + across.y * v };
    };
    const float hl = 0.5f * p.length, hw = 0.5f * p.width;
    switch (p.form)
    {
        case Form::Band: return;  // a planet's, never a module's
        case Form::Disc:
        case Form::Ring:
        case Form::Polygon: Include(lo, hi, p.pos, p.radius); return;
        case Form::Arc:
        {
            float from = std::min(p.arcFrom, p.arcTo), to = std::max(p.arcFrom, p.arcTo);
            if (to - from >= 360.0f)
            {
                Include(lo, hi, p.pos, p.radius);
                return;
            }
            const float inner = std::max(0.0f, p.radius - p.width);
            for (const float d : { from, to })
                for (const float r : { inner, p.radius })
                    Include(lo, hi,
                            { p.pos.x + std::cos(d * DEG2RAD) * r,
                              p.pos.y + std::sin(d * DEG2RAD) * r });
            // The outer edge bulges furthest where it crosses an axis.
            for (float d = std::ceil(from / 90.0f) * 90.0f; d <= to; d += 90.0f)
                Include(lo, hi,
                        { p.pos.x + std::cos(d * DEG2RAD) * p.radius,
                          p.pos.y + std::sin(d * DEG2RAD) * p.radius });
            return;
        }
        case Form::Bar:
        case Form::Lattice:
            for (const float u : { -hl, hl })
                for (const float v : { -hw, hw })
                    Include(lo, hi, at(u, v));
            return;
        case Form::Chevron:
            Include(lo, hi, at(hl, -hw * p.tip));
            Include(lo, hi, at(hl, hw * p.tip));
            Include(lo, hi, at(-hl, -hw));
            Include(lo, hi, at(-hl, hw));
            return;
        case Form::Capsule:
            Include(lo, hi, at(-hl, 0.0f), hw);
            Include(lo, hi, at(hl, 0.0f), hw);
            return;
    }
}

}  // namespace

Rectangle MeasuredBounds(const Shape& s)
{
    Pose pose;
    pose.size = 1.0f;
    pose.pixelsPerUnit = 1000.0f;  // every detail present, whatever its minPixels
    pose.thrusting = true;
    float lo[2] = { 1e9f, 1e9f }, hi[2] = { -1e9f, -1e9f };
    for (const Piece& p : Compose(s, pose))
        Measure(p, lo, hi);
    if (lo[0] > hi[0])
        return { -1.0f, -1.0f, 2.0f, 2.0f };
    return { lo[0], lo[1], hi[0] - lo[0], hi[1] - lo[1] };
}

namespace
{
std::vector<std::string> Strings(const json& o, const char* key)
{
    std::vector<std::string> out;
    if (o.contains(key) && o[key].is_array())
        for (const json& v : o[key])
            if (v.is_string())
                out.push_back(v.get<std::string>());
    return out;
}
}  // namespace

namespace Modules
{
void Clear()
{
    g_modules.clear();
    ForgetExpansions();
}

const std::vector<Module>& All()
{
    return g_modules;
}

const Module* Find(const std::string& id)
{
    for (const Module& m : g_modules)
        if (m.id == id)
            return &m;
    return nullptr;
}

namespace
{
// "data/modules/weapons.json" -> "weapons".
std::string Stem(const std::string& path)
{
    const size_t slash = path.find_last_of("/\\");
    std::string  name = slash == std::string::npos ? path : path.substr(slash + 1);
    const size_t dot = name.find_last_of('.');
    return dot == std::string::npos ? name : name.substr(0, dot);
}

bool LoadFile(const std::string& path, std::string& error)
{
    std::ifstream in(path);
    if (!in.is_open())
        return true;  // no library: nothing uses one
    const std::string pack = Stem(path);
    const json        data = json::parse(in, nullptr, false);
    if (data.is_discarded() || !data.is_object() || !data.contains("modules") ||
        !data["modules"].is_array())
    {
        error = path + ": not a module library ({ \"modules\": [...] })";
        return false;
    }
    for (const json& mj : data["modules"])
    {
        const std::string id = mj.is_object() ? mj.value("id", std::string()) : std::string();
        if (id.empty())
        {
            error = "a module needs an \"id\"";
            return false;
        }
        for (auto it = mj.begin(); it != mj.end(); ++it)
            if (it.key() != "id" && it.key() != "tags" && it.key() != "sockets" &&
                it.key() != "variants" && it.key() != "note" && it.key() != "handed")
            {
                error = "module '" + id + "': unknown field \"" + it.key() + "\"";
                return false;
            }
        if (const Module* twin = Find(id))
        {
            error = "module '" + id + "' is defined twice (" + twin->pack + ", " + pack + ")";
            return false;
        }
        Module m;
        m.id = id;
        m.pack = pack;
        m.tags = Strings(mj, "tags");
        m.sockets = Strings(mj, "sockets");
        if (mj.contains("handed") && !mj["handed"].is_boolean())
        {
            error = "module '" + id + "': \"handed\" is true or false";
            return false;
        }
        m.handed = mj.value("handed", false);
        if (!mj.contains("variants") || !mj["variants"].is_array() || mj["variants"].empty())
        {
            error = "module '" + id + "' has no variants";
            return false;
        }
        for (const json& vj : mj["variants"])
        {
            ModuleVariant v;
            v.id = vj.is_object() ? vj.value("id", std::string()) : std::string();
            if (vj.is_object())
                for (auto it = vj.begin(); it != vj.end(); ++it)
                    if (it.key() != "id" && it.key() != "shape" && it.key() != "note")
                    {
                        // A misspelt key on a variant would otherwise pass in silence.
                        error = "module '" + id + "' variant '" + v.id + "': unknown field \"" +
                                it.key() + "\"";
                        return false;
                    }
            if (v.id.empty() || !vj.contains("shape"))
            {
                error = "module '" + id + "': a variant needs an \"id\" and a \"shape\"";
                return false;
            }
            std::string why;
            if (!ParseShape(vj["shape"], v.shape, why))
            {
                error = "module '" + id + "' variant '" + v.id + "': " + why;
                return false;
            }
            for (const Part& p : v.shape.parts)
                if (p.repeat > 1 || p.mirror || p.orbitRadius > 0.0f || p.surface ||
                    !p.module.empty())
                {
                    // These place a part about the object's centre, which a module's parts
                    // know nothing about; and a module inside a module is not supported.
                    error = "module '" + id + "' variant '" + v.id +
                            "': a module's parts may not repeat, mirror, orbit, sit on a "
                            "sphere or be modules themselves";
                    return false;
                }
            v.bounds = MeasuredBounds(v.shape);
            m.variants.push_back(std::move(v));
        }
        g_modules.push_back(std::move(m));
    }
    return true;
}
}  // namespace

bool Load(const std::string& path, std::string& error)
{
    error.clear();
    g_modules.clear();
    ForgetExpansions();
    if (!LoadFile(path, error))
        return false;

    const size_t      slash = path.find_last_of("/\\");
    const std::string dir =
        (slash == std::string::npos ? std::string() : path.substr(0, slash + 1)) + "modules";
    if (!DirectoryExists(dir.c_str()))
        return true;
    std::vector<std::string> packs;
    FilePathList             files = LoadDirectoryFilesEx(dir.c_str(), ".json", false);
    for (unsigned int i = 0; i < files.count; i++)
        packs.push_back(files.paths[i]);
    UnloadDirectoryFiles(files);
    // Name order, so which file wins an argument does not depend on the file system.
    std::sort(packs.begin(), packs.end());
    for (const std::string& p : packs)
        if (!LoadFile(p, error))
        {
            error = Stem(p) + ": " + error;
            return false;
        }
    return true;
}
}  // namespace Modules
}  // namespace Render
