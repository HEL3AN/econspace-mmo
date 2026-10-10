#include "render/Treatment.h"

#include "render/Perf.h"
#include "render/Scene.h"
#include "rlgl.h"
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
    // Draw calls are counted where the passes are run; this is one full-target fill.
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
            TraceLog(LOG_WARNING, "Treatment: %s has no shader at %s -- pass dropped", PassName(k),
                     path.c_str());
            continue;
        }

        // The default vertex shader is raylib's own; only the fragment stage differs.
        Shader s = LoadShader(nullptr, path.c_str());
        // This is the case the whole class exists for. It is a property of the machine,
        // not of the build, so it is reported and stepped over. A shader that compiles
        // and then fails to link comes back as raylib's *default* shader rather than as
        // nothing, so it has to be compared against that too (#190): kept, it would be a
        // pass that silently draws the picture unchanged.
        const char* failed = nullptr;
        if (s.id == 0 || s.locs == nullptr)
            failed = "would not compile";
        else if (s.id == rlGetShaderIdDefault())
            failed = "would not link";
        if (failed != nullptr)
        {
            problems_.push_back(std::string(PassName(k)) + ": shader " + failed);
            TraceLog(LOG_WARNING, "Treatment: %s %s -- pass dropped", PassName(k), failed);
            continue;
        }

        Loaded l;
        l.kind = k;
        l.shader = s;
        l.locAmount = GetShaderLocation(s, "amount");
        l.locScale = GetShaderLocation(s, "scale");
        l.locResolution = GetShaderLocation(s, "resolution");
        l.locTime = GetShaderLocation(s, "time");
        l.locGlow = GetShaderLocation(s, "glow");
        shaders_.push_back(l);
    }

    // Bloom is three draws now (#296), and the two that make the glow have a shader of their
    // own. If that one fails, bloom goes with it -- said the same way as any pass that failed.
    if (Find(PassKind::Bloom) != nullptr)
    {
        const std::string path = dataDir + "shaders/bloom_blur.fs";
        const char*       failed = nullptr;
        Shader            s{};
        if (!FileExists(path.c_str()))
            failed = "has no blur shader";
        else
        {
            s = LoadShader(nullptr, path.c_str());
            if (s.id == 0 || s.locs == nullptr)
                failed = "blur shader would not compile";
            else if (s.id == rlGetShaderIdDefault())
                failed = "blur shader would not link";
        }
        if (failed != nullptr)
        {
            problems_.push_back(std::string("bloom: ") + failed);
            TraceLog(LOG_WARNING, "Treatment: bloom %s -- pass dropped", failed);
            for (size_t i = 0; i < shaders_.size(); i++)
                if (shaders_[i].kind == PassKind::Bloom)
                {
                    UnloadShader(shaders_[i].shader);
                    shaders_.erase(shaders_.begin() + (long)i);
                    break;
                }
        }
        else
        {
            blur_ = s;
            blurLoaded_ = true;
            locDirection_ = GetShaderLocation(s, "direction");
            locBrightPass_ = GetShaderLocation(s, "brightPass");
        }
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
    if (blurLoaded_)
        UnloadShader(blur_);
    blurLoaded_ = false;

    if (targetsReady_)
    {
        UnloadRenderTexture(scene_);
        UnloadRenderTexture(ping_);
        UnloadRenderTexture(pong_);
        UnloadRenderTexture(glowA_);
        UnloadRenderTexture(glowB_);
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
        UnloadRenderTexture(glowA_);
        UnloadRenderTexture(glowB_);
    }
    scene_ = LoadRenderTexture(width, height);
    ping_ = LoadRenderTexture(width, height);
    pong_ = LoadRenderTexture(width, height);
    // A glow is a blur, and a blur has no detail to lose at half resolution: a quarter of
    // the pixels, and bilinear filtering brings it back up smoothly.
    glowA_ = LoadRenderTexture(std::max(1, width / 2), std::max(1, height / 2));
    glowB_ = LoadRenderTexture(std::max(1, width / 2), std::max(1, height / 2));
    SetTextureFilter(glowA_.texture, TEXTURE_FILTER_BILINEAR);
    SetTextureFilter(glowB_.texture, TEXTURE_FILTER_BILINEAR);
    // Bilinear, so pixelation is a decision the shader makes rather than a side effect of
    // how the texture happens to be sampled.
    SetTextureFilter(scene_.texture, TEXTURE_FILTER_BILINEAR);
    SetTextureFilter(ping_.texture, TEXTURE_FILTER_BILINEAR);
    SetTextureFilter(pong_.texture, TEXTURE_FILTER_BILINEAR);
    width_ = width;
    height_ = height;
    targetsReady_ = true;
}

void Treatment::Begin(int width, int height, const RenderTexture2D* into)
{
    if (!config_.enabled || !Available() || width <= 0 || height <= 0)
        return;
    EnsureTargets(width, height);
    capturing_ = true;
    into_ = into;
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
    const float now = (float)LocalClock();

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
        // Bloom lays a glow made from the world as it was drawn over whatever the chain has
        // made of it so far. Every other pass reads one texture and is done.
        if (p.kind == PassKind::Bloom)
            Glow(p.scale);

        BeginTextureMode(*to);
        ClearBackground(BLANK);
        BeginShaderMode(l->shader);
        // The second sampler is set after everything that flushes raylib's batch -- starting
        // a texture mode does, and so does changing the shader -- because a flush forgets every
        // texture but the first. Set before them, as it was until #296, bloom read black and
        // drew nothing at all, while costing eighty-one taps a pixel.
        if (p.kind == PassKind::Bloom && l->locGlow >= 0)
            SetShaderValueTexture(l->shader, l->locGlow, glowB_.texture);
        DrawFull(from->texture, width_, height_);
        EndShaderMode();
        EndTextureMode();

        Perf::Count(Perf::Counter::Passes);
        from = to;
        to = (to == &ping_) ? &pong_ : &ping_;
    }

    // Whether any pass ran or not, whatever `from` points at is the picture: with an
    // empty chain that is the scene itself, which is the raw world drawn to a texture and
    // straight back out. That is the fallback working, not a bug.
    if (into_ != nullptr)
        BeginTextureMode(*into_);
    DrawFull(from->texture, width_, height_);
}

void Treatment::Glow(float scale)
{
    const int   w = glowA_.texture.width, h = glowA_.texture.height;
    const float bright = 1.0f, plain = 0.0f;
    // A step is `scale` pixels of the screen, as it was when this was one pass, whatever the
    // resolution it is taken at.
    const float across[2] = { scale / (float)width_, 0.0f };
    const float down[2] = { 0.0f, scale / (float)height_ };

    SetShaderValue(blur_, locDirection_, across, SHADER_UNIFORM_VEC2);
    SetShaderValue(blur_, locBrightPass_, &bright, SHADER_UNIFORM_FLOAT);
    BeginTextureMode(glowA_);
    ClearBackground(BLACK);
    BeginShaderMode(blur_);
    DrawFull(scene_.texture, w, h);
    EndShaderMode();
    EndTextureMode();

    SetShaderValue(blur_, locDirection_, down, SHADER_UNIFORM_VEC2);
    SetShaderValue(blur_, locBrightPass_, &plain, SHADER_UNIFORM_FLOAT);
    BeginTextureMode(glowB_);
    ClearBackground(BLACK);
    BeginShaderMode(blur_);
    DrawFull(glowA_.texture, w, h);
    EndShaderMode();
    EndTextureMode();
    Perf::Count(Perf::Counter::Passes, 2);
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
