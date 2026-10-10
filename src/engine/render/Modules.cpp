#include "render/Modules.h"

#include <fstream>
#include <nlohmann/json.hpp>

namespace Render
{
namespace
{
using json = nlohmann::json;
std::vector<Module> g_modules;

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

bool Load(const std::string& path, std::string& error)
{
    error.clear();
    g_modules.clear();
    std::ifstream in(path);
    if (!in.is_open())
        return true;  // no library: nothing uses one
    const json data = json::parse(in, nullptr, false);
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
                it.key() != "variants" && it.key() != "note")
            {
                error = "module '" + id + "': unknown field \"" + it.key() + "\"";
                return false;
            }
        if (Find(id) != nullptr)
        {
            error = "module '" + id + "' is defined twice";
            return false;
        }
        Module m;
        m.id = id;
        m.tags = Strings(mj, "tags");
        m.sockets = Strings(mj, "sockets");
        if (!mj.contains("variants") || !mj["variants"].is_array() || mj["variants"].empty())
        {
            error = "module '" + id + "' has no variants";
            return false;
        }
        for (const json& vj : mj["variants"])
        {
            ModuleVariant v;
            v.id = vj.is_object() ? vj.value("id", std::string()) : std::string();
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
            m.variants.push_back(std::move(v));
        }
        g_modules.push_back(std::move(m));
    }
    return true;
}
}  // namespace Modules
}  // namespace Render
