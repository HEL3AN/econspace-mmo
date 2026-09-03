#include "render/Material.h"

#include "render/Scene.h"
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
    { Source::LightDir, "light.dir" },
    { Source::LightTint, "light.tint" },
    { Source::LightStrength, "light.strength" },
    { Source::LightAmbient, "light.ambient" },
    { Source::ClockTime, "clock.time" },
};

std::vector<Material> g_materials;
std::string           g_error;

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

        case Source::LightDir: return Two(in.light.dir.x, in.light.dir.y);
        case Source::LightTint: return Four(in.light.tint);
        case Source::LightStrength: return One(in.light.strength);
        case Source::LightAmbient: return One(in.ambient);

        case Source::ClockTime: return One(in.time);
    }
    return UniformValue{};
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
            }
        }
        loaded.push_back(mat);
    }

    g_materials = std::move(loaded);
    return true;
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

}  // namespace Materials
}  // namespace Render
