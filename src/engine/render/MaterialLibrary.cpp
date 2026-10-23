#include "render/MaterialLibrary.h"

#include "core/Archetype.h"
#include "rlgl.h"
#include <cstdint>
#include <sstream>
#include <vector>

namespace Render
{

MaterialLibrary::~MaterialLibrary()
{
    Unload();
}

namespace
{
// A shader's text with every `#include "file"` line replaced by that file, from the same
// folder (#426). GLSL has no include of its own, and four materials that each carry a copy of
// the same noise are four copies to keep in step. One level deep: a library includes nothing.
// False, with the line that failed, if a file is missing -- which costs that material its
// shader and nothing else, like any shader that will not compile.
bool ReadWithIncludes(const std::string& path, const std::string& dir, std::string& out,
                      std::string& missing)
{
    char* text = LoadFileText(path.c_str());
    if (text == nullptr)
    {
        missing = path;
        return false;
    }
    std::istringstream in(text);
    UnloadFileText(text);
    std::string line;
    out.clear();
    while (std::getline(in, line))
    {
        const size_t at = line.find("#include \"");
        if (at == std::string::npos || line.find_first_not_of(" \t") != at)
        {
            out += line + "\n";
            continue;
        }
        const size_t      from = at + 10, to = line.find('"', from);
        const std::string name = to == std::string::npos ? "" : line.substr(from, to - from);
        char*             lib = name.empty() ? nullptr : LoadFileText((dir + name).c_str());
        if (lib == nullptr)
        {
            missing = dir + name;
            return false;
        }
        out += lib;
        out += "\n";
        UnloadFileText(lib);
    }
    return true;
}
}  // namespace

// The noise table (see NOISE_SIZE). A linear congruential generator rather than <random>:
// its sequence is written down, so the table is the same whatever the standard library.
static Texture2D MakeNoiseTexture()
{
    const int                  n = MaterialLibrary::NOISE_SIZE;
    std::vector<unsigned char> red((size_t)(n * n));
    uint32_t                   state = 0x2545F491u;
    for (unsigned char& v : red)
    {
        state = state * 1664525u + 1013904223u;
        v = (unsigned char)(state >> 24);
    }
    Image  img = GenImageColor(n, n, BLANK);
    Color* px = (Color*)img.data;
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++)
            px[y * n + x] = { red[(size_t)(y * n + x)],
                              red[(size_t)(((y + 17) % n) * n + (x + 37) % n)], 0, 255 };
    Texture2D t = LoadTextureFromImage(img);
    UnloadImage(img);
    // Read with texelFetch, so the filter never applies; nearest says so to anyone looking.
    SetTextureFilter(t, TEXTURE_FILTER_POINT);
    SetTextureWrap(t, TEXTURE_WRAP_REPEAT);
    return t;
}

void MaterialLibrary::Load(const std::string& dataDir)
{
    Unload();
    if (disabled_)
    {
        problems_.push_back("materials switched off: every object drawn plain");
        TraceLog(LOG_INFO, "Materials: switched off -- drawing every object plain");
        return;
    }

    // Said here because this is the one place every client and the editor pass through
    // with both registries loaded.
    for (const std::string& p : Materials::UnknownInArchetypes())
    {
        problems_.push_back(p);
        TraceLog(LOG_ERROR, "Materials: %s", p.c_str());
    }

    // One program per shader file, however many materials bind it (#516): the built surfaces
    // are one shader under several sets of constants, and every Begin sets all of a
    // material's uniforms anyway, so they can share it. Compiling the same text eight times
    // is startup time on a laptop for nothing.
    std::map<std::string, Shader> programs;
    for (const Material& m : Materials::All())
    {
        if (paintingOff_)
        {
            // The likelier driver failure (#426): only the plain materials are left. A
            // painting is a material no archetype wears as a whole, or one with a fallback of
            // its own (#516) -- which is then what is drawn in its place.
            bool worn = false;
            for (const Archetype& a : Archetypes::All())
                worn = worn || a.visual.material == m.id;
            if (!worn || !m.fallback.empty())
            {
                problems_.push_back(m.id + ": painting switched off");
                continue;
            }
        }
        if (const auto shared = programs.find(m.shader); shared != programs.end())
        {
            Compiled c;
            c.shader = shared->second;
            for (const Binding& b : m.bindings)
            {
                c.locations[b.uniform] = GetShaderLocation(c.shader, b.uniform.c_str());
                c.slots.push_back(c.locations[b.uniform]);
            }
            shaders_[m.id] = c;
            continue;
        }
        const std::string path = dataDir + "shaders/materials/" + m.shader + ".fs";
        if (!FileExists(path.c_str()))
        {
            problems_.push_back(m.id + ": no " + path);
            TraceLog(LOG_WARNING, "Material '%s': no shader at %s -- drawing plain", m.id.c_str(),
                     path.c_str());
            continue;
        }

        std::string code, missing;
        if (!ReadWithIncludes(path, dataDir + "shaders/materials/", code, missing))
        {
            problems_.push_back(m.id + ": cannot read " + missing);
            TraceLog(LOG_WARNING, "Material '%s': cannot read %s -- drawing plain", m.id.c_str(),
                     missing.c_str());
            continue;
        }
        Shader s = LoadShaderFromMemory(nullptr, code.c_str());
        // A shader that compiles but will not link comes back as raylib's default shader,
        // not as nothing (#190). Counted as loaded, the object would be drawn flat and
        // unlit by the default shader, with the log saying every material had one.
        const char* failed = nullptr;
        if (s.id == 0 || s.locs == nullptr)
            failed = "would not compile";
        else if (s.id == rlGetShaderIdDefault())
            failed = "would not link";
        if (failed != nullptr)
        {
            problems_.push_back(m.id + ": shader " + failed);
            TraceLog(LOG_WARNING, "Material '%s': shader %s -- drawing plain", m.id.c_str(),
                     failed);
            continue;
        }

        Compiled c;
        c.shader = s;
        // Looked up once. A uniform the shader does not declare gets -1 and is skipped
        // every frame afterwards, which is what lets a material list more bindings than a
        // particular shader uses.
        for (const Binding& b : m.bindings)
        {
            c.locations[b.uniform] = GetShaderLocation(s, b.uniform.c_str());
            c.slots.push_back(c.locations[b.uniform]);
        }
        shaders_[m.id] = c;
        programs[m.shader] = s;
    }

    if (!shaders_.empty())
        noise_ = MakeNoiseTexture();

    if (!Materials::All().empty())
        TraceLog(LOG_INFO, "Materials: %d of %d have a shader", (int)shaders_.size(),
                 (int)Materials::All().size());
}

void MaterialLibrary::Unload()
{
    // Materials may share a program (see Load); each program is unloaded once.
    std::vector<unsigned int> unloaded;
    for (auto& entry : shaders_)
    {
        bool done = false;
        for (unsigned int id : unloaded)
            done = done || id == entry.second.shader.id;
        if (done)
            continue;
        unloaded.push_back(entry.second.shader.id);
        UnloadShader(entry.second.shader);
    }
    shaders_.clear();
    constantsOn_.clear();
    problems_.clear();
    if (noise_.id != 0)
        UnloadTexture(noise_);
    noise_ = Texture2D{};
    if (paint_.id != 0)
        UnloadRenderTexture(paint_);
    paint_ = RenderTexture2D{};
    active_ = false;
}

bool MaterialLibrary::Begin(const Material& m, const MaterialInputs& in)
{
    auto it = shaders_.find(m.id);
    if (it == shaders_.end())
        return false;

    Compiled& c = it->second;
    // Its constants are already on the program if this material was the last to set them
    // (#516): a station's parts bind the same material dozens of times a frame, and the
    // materials of one shader share its program, so only a change of material resets them.
    const Material*& last = constantsOn_[c.shader.id];
    const bool       sameConstants = last == &m;
    last = &m;
    // The program once, then its uniforms: SetShaderValue makes the program current for every
    // value it sets, which was a glUseProgram per uniform per part.
    rlEnableShader(c.shader.id);
    for (size_t i = 0; i < m.bindings.size() && i < c.slots.size(); i++)
    {
        const Binding& b = m.bindings[i];
        const int      loc = c.slots[i];
        if (loc < 0 || (sameConstants && b.source == Source::Constant))
            continue;
        if (b.source == Source::TextureNoise)
        {
            // Bound to a unit of its own and the sampler pointed at it. Every time, not once:
            // anything else that binds a texture there would otherwise be read as noise.
            const int unit = NOISE_UNIT;
            rlActiveTextureSlot(unit);
            rlEnableTexture(noise_.id);
            rlActiveTextureSlot(0);
            rlSetUniform(loc, &unit, SHADER_UNIFORM_INT, 1);
            continue;
        }
        const UniformValue u = Resolve(b, in);
        int                type = SHADER_UNIFORM_FLOAT;
        if (u.count == 2)
            type = SHADER_UNIFORM_VEC2;
        else if (u.count == 3)
            type = SHADER_UNIFORM_VEC3;
        else if (u.count == 4)
            type = SHADER_UNIFORM_VEC4;
        rlSetUniform(loc, u.v, type, 1);
    }

    BeginShaderMode(c.shader);
    active_ = true;
    return true;
}

RenderTexture2D* MaterialLibrary::PaintTarget(int width, int height)
{
    if (width <= 0 || height <= 0)
        return nullptr;
    if (paint_.id != 0 && (paint_.texture.width != width || paint_.texture.height != height))
    {
        UnloadRenderTexture(paint_);
        paint_ = RenderTexture2D{};
    }
    if (paint_.id == 0)
    {
        paint_ = LoadRenderTexture(width, height);
        if (paint_.id == 0)
            return nullptr;
        // Upscaled when it is drawn, so filtered: nearest would show its pixels as blocks.
        SetTextureFilter(paint_.texture, TEXTURE_FILTER_BILINEAR);
    }
    return &paint_;
}

void MaterialLibrary::End()
{
    if (!active_)
        return;
    active_ = false;
    EndShaderMode();
}

}  // namespace Render
