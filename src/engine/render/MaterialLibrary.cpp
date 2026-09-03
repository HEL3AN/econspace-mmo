#include "render/MaterialLibrary.h"

namespace Render
{

MaterialLibrary::~MaterialLibrary()
{
    Unload();
}

void MaterialLibrary::Load(const std::string& dataDir)
{
    Unload();

    for (const Material& m : Materials::All())
    {
        const std::string path = dataDir + "shaders/materials/" + m.shader + ".fs";
        if (!FileExists(path.c_str()))
        {
            problems_.push_back(m.id + ": no " + path);
            continue;
        }

        Shader s = LoadShader(nullptr, path.c_str());
        if (s.id == 0 || s.locs == nullptr)
        {
            problems_.push_back(m.id + ": shader would not compile");
            TraceLog(LOG_WARNING, "Material '%s': shader would not compile -- drawing plain",
                     m.id.c_str());
            continue;
        }

        Compiled c;
        c.shader = s;
        // Looked up once. A uniform the shader does not declare gets -1 and is skipped
        // every frame afterwards, which is what lets a material list more bindings than a
        // particular shader uses.
        for (const Binding& b : m.bindings)
            c.locations[b.uniform] = GetShaderLocation(s, b.uniform.c_str());
        shaders_[m.id] = c;
    }

    if (!Materials::All().empty())
        TraceLog(LOG_INFO, "Materials: %d of %d have a shader", (int)shaders_.size(),
                 (int)Materials::All().size());
}

void MaterialLibrary::Unload()
{
    for (auto& entry : shaders_)
        UnloadShader(entry.second.shader);
    shaders_.clear();
    problems_.clear();
    active_ = false;
}

bool MaterialLibrary::Begin(const Material& m, const MaterialInputs& in)
{
    auto it = shaders_.find(m.id);
    if (it == shaders_.end())
        return false;

    Compiled& c = it->second;
    for (const Binding& b : m.bindings)
    {
        const int loc = c.locations[b.uniform];
        if (loc < 0)
            continue;
        const UniformValue u = Resolve(b, in);
        int                type = SHADER_UNIFORM_FLOAT;
        if (u.count == 2)
            type = SHADER_UNIFORM_VEC2;
        else if (u.count == 3)
            type = SHADER_UNIFORM_VEC3;
        else if (u.count == 4)
            type = SHADER_UNIFORM_VEC4;
        SetShaderValue(c.shader, loc, u.v, type);
    }

    BeginShaderMode(c.shader);
    active_ = true;
    return true;
}

void MaterialLibrary::End()
{
    if (!active_)
        return;
    active_ = false;
    EndShaderMode();
}

}  // namespace Render
