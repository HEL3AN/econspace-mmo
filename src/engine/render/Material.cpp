#include "render/Material.h"

#include "core/Archetype.h"
#include "render/Modules.h"
#include "render/Scene.h"
#include "render/Silhouette.h"
#include <nlohmann/json.hpp>
#include <fstream>

using nlohmann::json;

namespace Render
{
namespace
{
struct NamedSource
{
    Source      source;
    const char* name;
};

const NamedSource SOURCES[] = {
    { Source::ItemColor, "item.color" },
    { Source::ItemIntensity, "item.intensity" },
    { Source::ItemHeading, "item.heading" },
    { Source::ItemThrusting, "item.thrusting" },
    { Source::ItemSize, "item.size" },
    { Source::ItemScreenPos, "item.screenPos" },
    { Source::ItemScreenSize, "item.screenSize" },
    { Source::ItemAxis, "item.axis" },
    { Source::ItemSeed, "item.seed" },
    { Source::ItemPartSeed, "item.partSeed" },
    { Source::ItemAngle, "item.angle" },
    { Source::ItemPartShape, "item.partShape" },
    { Source::ItemFrame, "item.frame" },
    { Source::LightDir, "light.dir" },
    { Source::LightTint, "light.tint" },
    { Source::LightSurface, "light.surface" },
    { Source::LightStrength, "light.strength" },
    { Source::LightAmbient, "light.ambient" },
    { Source::LiveryAccent, "livery.accent" },
    { Source::LiveryParams, "livery.params" },
    { Source::LiveryStyle, "livery.style" },
    { Source::ClockTime, "clock.time" },
    { Source::TextureNoise, "texture.noise" },
};

std::vector<Material> g_materials;
std::vector<Livery>   g_liveries;
std::string           g_error;

// What a material is given when the object has no livery and the file defines no default.
const Livery NEUTRAL_LIVERY{};

UniformValue FourOf(const float* f)
{
    UniformValue u;
    u.count = 4;
    for (int i = 0; i < 4; i++)
        u.v[i] = f[i];
    return u;
}

// A colour in a livery: three or four bytes, as an archetype writes one.
bool ReadColor(const json& j, Color& out)
{
    if (!j.is_array() || (j.size() != 3 && j.size() != 4))
        return false;
    unsigned char c[4] = { 0, 0, 0, 255 };
    for (size_t i = 0; i < j.size(); i++)
    {
        if (!j[i].is_number() || j[i].get<float>() < 0.0f || j[i].get<float>() > 255.0f)
            return false;
        c[i] = (unsigned char)j[i].get<float>();
    }
    out = { c[0], c[1], c[2], c[3] };
    return true;
}

UniformValue One(float a)
{
    UniformValue u;
    u.count = 1;
    u.v[0] = a;
    return u;
}

UniformValue Two(float a, float b)
{
    UniformValue u;
    u.count = 2;
    u.v[0] = a;
    u.v[1] = b;
    return u;
}

UniformValue Four(Color c)
{
    UniformValue u;
    u.count = 4;
    u.v[0] = c.r / 255.0f;
    u.v[1] = c.g / 255.0f;
    u.v[2] = c.b / 255.0f;
    u.v[3] = c.a / 255.0f;
    return u;
}

// A constant in the data is one number or a short array of them. Anything else is an
// error the loader reports rather than a default nobody asked for.
bool ReadConstant(const json& j, UniformValue& out, std::string& error)
{
    if (j.is_number())
    {
        out = One(j.get<float>());
        return true;
    }
    if (j.is_boolean())
    {
        out = One(j.get<bool>() ? 1.0f : 0.0f);
        return true;
    }
    if (j.is_array() && !j.empty() && j.size() <= 4)
    {
        out.count = (int)j.size();
        for (size_t i = 0; i < j.size(); i++)
        {
            if (!j[i].is_number())
            {
                error = "a constant must be numbers";
                return false;
            }
            out.v[i] = j[i].get<float>();
        }
        return true;
    }
    error = "a constant must be a number, a bool, or one to four numbers";
    return false;
}
}  // namespace

const char* SourceName(Source s)
{
    for (const NamedSource& n : SOURCES)
        if (n.source == s)
            return n.name;
    return "constant";
}

bool SourceFromName(const std::string& s, Source& out)
{
    for (const NamedSource& n : SOURCES)
        if (s == n.name)
        {
            out = n.source;
            return true;
        }
    return false;
}

float SeedOf(int id)
{
    // Knuth's multiplier, then the top 24 bits: what a float holds exactly, so a seed is the
    // same number on every machine.
    const unsigned h = (unsigned)id * 2654435761u;
    return (float)(h >> 8) / 16777216.0f;
}

UniformValue Resolve(const Binding& b, const MaterialInputs& in)
{
    // A material asked for something about an object it was not given. Zero rather than a
    // read through null: this should draw wrong, not stop the game.
    const bool haveItem = in.item != nullptr;

    switch (b.source)
    {
        case Source::Constant: return b.constant;

        case Source::ItemColor: return haveItem ? Four(in.item->color) : UniformValue{ 4, {} };
        case Source::ItemIntensity: return One(haveItem ? in.item->intensity : 0.0f);
        case Source::ItemHeading: return One(haveItem ? in.item->heading : 0.0f);
        case Source::ItemThrusting: return One(haveItem && in.item->thrusting ? 1.0f : 0.0f);
        case Source::ItemSize: return One(haveItem ? in.item->size : 0.0f);
        case Source::ItemScreenPos: return Two(in.screenPos.x, in.screenPos.y);
        case Source::ItemScreenSize: return One(in.screenSize);
        case Source::ItemAxis: return Two(in.axis.x, in.axis.y);
        case Source::ItemSeed: return One(haveItem ? SeedOf(in.item->id) : 0.0f);
        case Source::ItemPartSeed: return One(in.partSeed);
        case Source::ItemAngle: return One(in.angle);
        case Source::ItemPartShape: return FourOf(in.partShape);
        case Source::ItemFrame: return FourOf(in.frame);

        case Source::LightDir: return Two(in.light.dir.x, in.light.dir.y);
        case Source::LightTint: return Four(in.light.tint);
        case Source::LightSurface: return Four(in.light.surface);
        case Source::LightStrength: return One(in.light.strength);
        case Source::LightAmbient: return One(in.ambient);

        case Source::LiveryAccent:
            return Four(in.livery != nullptr ? in.livery->accent : NEUTRAL_LIVERY.accent);
        case Source::LiveryParams:
            return FourOf(in.livery != nullptr ? in.livery->params : NEUTRAL_LIVERY.params);
        case Source::LiveryStyle:
            return FourOf(in.livery != nullptr ? in.livery->style : NEUTRAL_LIVERY.style);

        case Source::ClockTime: return One(in.time);

        case Source::TextureNoise: return UniformValue{ 0, {} };  // bound by the library
    }
    return UniformValue{};
}

const std::string& Material::ForRole(const std::string& role) const
{
    for (const auto& r : roles)
        if (r.first == role)
            return r.second;
    return id;
}

namespace Materials
{

bool Load(const std::string& path)
{
    g_error.clear();
    std::ifstream in(path);
    if (!in.is_open())
    {
        g_error = "cannot open " + path;
        return false;
    }

    json j = json::parse(in, nullptr, false);
    if (j.is_discarded() || !j.contains("materials") || !j["materials"].is_array())
    {
        g_error = path + " has no materials array";
        return false;
    }

    std::vector<Material> loaded;
    for (const json& m : j["materials"])
    {
        Material mat;
        mat.id = m.value("id", std::string());
        mat.shader = m.value("shader", mat.id);
        if (m.contains("reach"))
        {
            if (!m["reach"].is_number() || m["reach"].get<float>() < 1.0f ||
                m["reach"].get<float>() > 8.0f)
            {
                g_error = "material '" + m.value("id", std::string()) +
                          "': reach is how far past the part it draws, 1 to 8 of its radii";
                return false;
            }
            mat.reach = m["reach"].get<float>();
        }
        if (mat.id.empty())
        {
            g_error = "a material has no id";
            return false;
        }

        if (m.contains("bindings") && m["bindings"].is_object())
        {
            for (auto it = m["bindings"].begin(); it != m["bindings"].end(); ++it)
            {
                Binding b;
                b.uniform = it.key();
                if (it.value().is_string())
                {
                    // A material naming a source that does not exist is rejected rather
                    // than losing that binding quietly. A look setting can afford to lose
                    // a pass -- the picture gets plainer. A material that loses a uniform
                    // draws something actively wrong, and would do it silently.
                    if (!SourceFromName(it.value().get<std::string>(), b.source))
                    {
                        g_error = "material '" + mat.id + "': unknown source '" +
                                  it.value().get<std::string>() + "' for uniform '" + b.uniform +
                                  "'";
                        return false;
                    }
                }
                else
                {
                    b.source = Source::Constant;
                    std::string why;
                    if (!ReadConstant(it.value(), b.constant, why))
                    {
                        g_error = "material '" + mat.id + "', uniform '" + b.uniform + "': " + why;
                        return false;
                    }
                }
                mat.bindings.push_back(b);
                mat.readsPart = mat.readsPart || b.source == Source::ItemPartSeed ||
                                b.source == Source::ItemAngle || b.source == Source::ItemPartShape;
            }
        }
        if (m.contains("roles"))
        {
            if (!m["roles"].is_object())
            {
                g_error = "material '" + mat.id + "': roles maps a role to a material";
                return false;
            }
            for (auto it = m["roles"].begin(); it != m["roles"].end(); ++it)
            {
                Role role;
                if (!RoleFromName(it.key(), role) || !it.value().is_string())
                {
                    g_error = "material '" + mat.id + "': roles: '" + it.key() +
                              "' is not a role, or names no material";
                    return false;
                }
                mat.roles.emplace_back(it.key(), it.value().get<std::string>());
            }
        }
        if (m.contains("fallback"))
        {
            if (!m["fallback"].is_string())
            {
                g_error = "material '" + mat.id + "': fallback names a material";
                return false;
            }
            mat.fallback = m["fallback"].get<std::string>();
        }
        loaded.push_back(mat);
    }

    // Every material a material names must be in the file: a typo here would quietly draw a
    // radiator as plating, or a station plain on the one driver whose shader failed.
    auto defined = [&](const std::string& id)
    {
        for (const Material& m : loaded)
            if (m.id == id)
                return true;
        return false;
    };
    for (const Material& m : loaded)
    {
        for (const auto& r : m.roles)
            if (!defined(r.second))
            {
                g_error = "material '" + m.id + "': role '" + r.first + "' names material '" +
                          r.second + "', which is not defined";
                return false;
            }
        if (!m.fallback.empty() && !defined(m.fallback))
        {
            g_error =
                "material '" + m.id + "': falls back to '" + m.fallback + "', which is not defined";
            return false;
        }
    }

    std::vector<Livery> liveries;
    if (j.contains("liveries"))
    {
        if (!j["liveries"].is_array())
        {
            g_error = path + ": liveries is not an array";
            return false;
        }
        for (const json& l : j["liveries"])
        {
            Livery lv;
            lv.id = l.value("id", std::string());
            const std::string who = "livery '" + lv.id + "': ";
            if (lv.id.empty())
            {
                g_error = "a livery has no id";
                return false;
            }
            for (auto it = l.begin(); it != l.end(); ++it)
            {
                const std::string& k = it.key();
                if (k == "id" || k == "note")
                    continue;
                if (k == "base" || k == "accent")
                {
                    if (!ReadColor(it.value(), k == "base" ? lv.base : lv.accent))
                    {
                        g_error = who + k + " is three or four numbers, 0 to 255";
                        return false;
                    }
                    continue;
                }
                static const char* const PARAMS[] = { "wear", "grime", "stripes", "panels" };
                static const char* const STYLE[] = { "aspect", "patchwork", "warmth", "windows" };
                float*                   slot = nullptr;
                for (int i = 0; i < 4; i++)
                {
                    if (k == PARAMS[i])
                        slot = &lv.params[i];
                    if (k == STYLE[i])
                        slot = &lv.style[i];
                }
                // Strict like the archetypes (#191): a misspelt field would otherwise be a
                // livery that quietly kept its default.
                if (slot == nullptr || !it.value().is_number())
                {
                    g_error = who + "unknown field '" + k + "', or not a number";
                    return false;
                }
                *slot = it.value().get<float>();
            }
            liveries.push_back(lv);
        }
    }

    g_materials = std::move(loaded);
    g_liveries = std::move(liveries);
    return true;
}

const Livery* FindLivery(const std::string& id)
{
    const Livery* fallback = nullptr;
    for (const Livery& l : g_liveries)
    {
        if (l.id == id)
            return &l;
        if (l.id == "default")
            fallback = &l;
    }
    return fallback;
}

const std::vector<Livery>& Liveries()
{
    return g_liveries;
}

const Material* Find(const std::string& id)
{
    for (const Material& m : g_materials)
        if (m.id == id)
            return &m;
    return nullptr;
}

const std::vector<Material>& All()
{
    return g_materials;
}

const std::string& Error()
{
    return g_error;
}

std::vector<std::string> UnknownInArchetypes()
{
    std::vector<std::string> out;
    for (const Archetype& a : Archetypes::All())
    {
        if (!a.visual.material.empty() && Find(a.visual.material) == nullptr)
            out.push_back("archetype '" + a.id + "' names material '" + a.visual.material +
                          "', which is not defined -- drawing it plain");
        // A part's own (#426), written in the archetype or in a module it places.
        std::vector<std::string> named;
        for (const Part& p : a.visual.shape.parts)
        {
            if (!p.material.empty())
                named.push_back(p.material);
            if (const Module* m = p.module.empty() ? nullptr : Modules::Find(p.module))
                for (const ModuleVariant& v : m->variants)
                    for (const Part& q : v.shape.parts)
                        if (!q.material.empty())
                            named.push_back(q.material);
        }
        for (size_t i = 0; i < named.size(); i++)
        {
            bool said = false;
            for (size_t k = 0; k < i; k++)
                said = said || named[k] == named[i];
            if (!said && Find(named[i]) == nullptr)
                out.push_back("archetype '" + a.id + "' has a part on material '" + named[i] +
                              "', which is not defined -- drawing it as the object");
        }
    }
    // Every module in the library, too (#516): a kit places them by tag and by the seed, so
    // which archetype wears a radiator is not written anywhere a check above would see.
    for (const Module& m : Modules::All())
        for (const ModuleVariant& v : m.variants)
            for (const Part& q : v.shape.parts)
                if (!q.material.empty() && Find(q.material) == nullptr)
                    out.push_back("module '" + m.id + "' (" + v.id + ") has a part on material '" +
                                  q.material +
                                  "', which is not defined -- drawing it as the object");
    return out;
}

}  // namespace Materials
}  // namespace Render
