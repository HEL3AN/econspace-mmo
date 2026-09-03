#include "render/Treatment.h"

#include <algorithm>

namespace Render
{
namespace
{
// A pass draws the whole of one texture over the whole of another. raylib's render
// textures are stored bottom-up, so the source rectangle has a negative height; getting
// this wrong shows as a world that is upside down, which is at least obvious.
void DrawFull(const Texture2D& tex, int w, int h)
{
    const Rectangle src{ 0.0f, 0.0f, (float)tex.width, -(float)tex.height };
    const Rectangle dst{ 0.0f, 0.0f, (float)w, (float)h };
    DrawTexturePro(tex, src, dst, { 0.0f, 0.0f }, 0.0f, WHITE);
}
}  // namespace

Treatment::~Treatment()
{
    Unload();
}

void Treatment::Load(const std::string& dataDir)
{
    Unload();
    dataDir_ = dataDir;

    std::string error;
    if (!LoadTreatment(dataDir + "look.json", config_, error))
    {
        // Not a problem worth showing a player: shipping without the file is normal, and
        // the defaults are the look as designed.
        TraceLog(LOG_INFO, "Treatment: %s -- using the default chain", error.c_str());
        config_ = TreatmentConfig::Default();
    }

    for (PassKind k : AllPasses())
    {
        const std::string path = dataDir + "shaders/" + PassName(k) + ".fs";
        if (!FileExists(path.c_str()))
        {
            problems_.push_back(std::string(PassName(k)) + ": no " + path);
            continue;
        }

        // The default vertex shader is raylib's own; only the fragment stage differs.
        Shader s = LoadShader(nullptr, path.c_str());
        if (s.id == 0 || s.locs == nullptr)
        {
            // This is the case the whole class exists for. It is a property of the
            // machine, not of the build, so it is reported and stepped over.
            problems_.push_back(std::string(PassName(k)) + ": shader would not compile");
            TraceLog(LOG_WARNING, "Treatment: %s would not compile -- pass dropped", PassName(k));
            continue;
        }

        Loaded l;
        l.kind = k;
        l.shader = s;
        l.locAmount = GetShaderLocation(s, "amount");
        l.locScale = GetShaderLocation(s, "scale");
        l.locResolution = GetShaderLocation(s, "resolution");
        l.locTime = GetShaderLocation(s, "time");
        l.locScene = GetShaderLocation(s, "scene");
        shaders_.push_back(l);
    }

    if (shaders_.empty())
        TraceLog(LOG_WARNING, "Treatment: no passes loaded -- drawing without the chain");
    else
        TraceLog(LOG_INFO, "Treatment: %d of %d passes loaded", (int)shaders_.size(),
                 (int)AllPasses().size());
}

void Treatment::Unload()
{
    for (Loaded& l : shaders_)
        UnloadShader(l.shader);
    shaders_.clear();
    problems_.clear();

    if (targetsReady_)
    {
        UnloadRenderTexture(scene_);
        UnloadRenderTexture(ping_);
        UnloadRenderTexture(pong_);
        targetsReady_ = false;
    }
    capturing_ = false;
}

const Treatment::Loaded* Treatment::Find(PassKind k) const
{
    for (const Loaded& l : shaders_)
        if (l.kind == k)
            return &l;
    return nullptr;
}

bool Treatment::Compiled(PassKind k) const
{
    return Find(k) != nullptr;
}

void Treatment::EnsureTargets(int width, int height)
{
    if (targetsReady_ && width == width_ && height == height_)
        return;
    if (targetsReady_)
    {
        UnloadRenderTexture(scene_);
        UnloadRenderTexture(ping_);
        UnloadRenderTexture(pong_);
    }
    scene_ = LoadRenderTexture(width, height);
    ping_ = LoadRenderTexture(width, height);
    pong_ = LoadRenderTexture(width, height);
    // Bilinear, so pixelation is a decision the shader makes rather than a side effect of
    // how the texture happens to be sampled.
    SetTextureFilter(scene_.texture, TEXTURE_FILTER_BILINEAR);
    SetTextureFilter(ping_.texture, TEXTURE_FILTER_BILINEAR);
    SetTextureFilter(pong_.texture, TEXTURE_FILTER_BILINEAR);
    width_ = width;
    height_ = height;
    targetsReady_ = true;
}

void Treatment::Begin(int width, int height)
{
    if (!config_.enabled || !Available() || width <= 0 || height <= 0)
        return;
    EnsureTargets(width, height);
    capturing_ = true;
    BeginTextureMode(scene_);
    ClearBackground(BLACK);
}

void Treatment::End()
{
    if (!capturing_)
        return;
    capturing_ = false;
    EndTextureMode();

    const float resolution[2] = { (float)width_, (float)height_ };
    const float now = (float)GetTime();

    // Ping-pong between two targets. `from` is what the last pass produced; the first
    // reads the scene itself.
    RenderTexture2D* from = &scene_;
    RenderTexture2D* to = &ping_;

    for (const Pass& p : config_.chain)
    {
        if (!p.enabled || p.amount <= 0.0f)
            continue;
        const Loaded* l = Find(p.kind);
        if (l == nullptr)
            continue;

        if (l->locAmount >= 0)
            SetShaderValue(l->shader, l->locAmount, &p.amount, SHADER_UNIFORM_FLOAT);
        if (l->locScale >= 0)
            SetShaderValue(l->shader, l->locScale, &p.scale, SHADER_UNIFORM_FLOAT);
        if (l->locResolution >= 0)
            SetShaderValue(l->shader, l->locResolution, resolution, SHADER_UNIFORM_VEC2);
        if (l->locTime >= 0)
            SetShaderValue(l->shader, l->locTime, &now, SHADER_UNIFORM_FLOAT);
        // Bloom composites its blur back over the picture as it stood before the blur, so
        // it needs both. Every other pass reads one texture and is done.
        if (l->locScene >= 0)
            SetShaderValueTexture(l->shader, l->locScene, scene_.texture);

        BeginTextureMode(*to);
        ClearBackground(BLANK);
        BeginShaderMode(l->shader);
        DrawFull(from->texture, width_, height_);
        EndShaderMode();
        EndTextureMode();

        from = to;
        to = (to == &ping_) ? &pong_ : &ping_;
    }

    // Whether any pass ran or not, whatever `from` points at is the picture: with an
    // empty chain that is the scene itself, which is the raw world drawn to a texture and
    // straight back out. That is the fallback working, not a bug.
    DrawFull(from->texture, width_, height_);
}

bool Treatment::Save(std::string& error) const
{
    if (dataDir_.empty())
    {
        error = "no data directory -- Load was never called";
        return false;
    }
    return SaveTreatment(dataDir_ + "look.json", config_, error);
}

}  // namespace Render
