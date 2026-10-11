#include "render/Backdrop.h"

#include "render/Perf.h"
#include "rlgl.h"

#include <algorithm>

namespace Render
{

namespace
{
// A render texture is stored upside down; drawn with a negative height it lands upright,
// and every target here keeps that same convention.
void DrawInto(const Texture2D& from, const RenderTexture2D& to)
{
    BeginTextureMode(to);
    ClearBackground(BLACK);
    DrawTexturePro(from, { 0.0f, 0.0f, (float)from.width, -(float)from.height },
                   { 0.0f, 0.0f, (float)to.texture.width, (float)to.texture.height },
                   { 0.0f, 0.0f }, 0.0f, WHITE);
    EndTextureMode();
}

RenderTexture2D Target(int w, int h)
{
    RenderTexture2D t = LoadRenderTexture(std::max(1, w), std::max(1, h));
    SetTextureFilter(t.texture, TEXTURE_FILTER_BILINEAR);
    return t;
}
}  // namespace

Backdrop::~Backdrop()
{
    // Nothing here outlives the window: Unload is the caller's, before CloseWindow.
}

void Backdrop::Load(const std::string& dataDir)
{
    Unload();
    const std::string path = dataDir + "shaders/bloom_blur.fs";
    if (FileExists(path.c_str()))
    {
        Shader s = LoadShader(nullptr, path.c_str());
        if (s.id != 0 && s.locs != nullptr && s.id != rlGetShaderIdDefault())
        {
            blur_ = s;
            shaderLoaded_ = true;
            locDirection_ = GetShaderLocation(s, "direction");
            locBrightPass_ = GetShaderLocation(s, "brightPass");
        }
    }
    if (!shaderLoaded_)
        TraceLog(LOG_WARNING, "Backdrop: no blur shader -- glass windows get a coarser blur");
}

void Backdrop::Unload()
{
    FreeTargets();
    if (shaderLoaded_)
        UnloadShader(blur_);
    shaderLoaded_ = false;
}

void Backdrop::FreeTargets()
{
    if (!targetsReady_)
        return;
    for (RenderTexture2D* t :
         { &scene_, &half_, &quarter_, &blurA_, &blurB_, &eighth_, &sixteenth_ })
        UnloadRenderTexture(*t);
    targetsReady_ = false;
}

void Backdrop::EnsureTargets(int width, int height)
{
    if (targetsReady_ && width == width_ && height == height_)
        return;
    FreeTargets();
    scene_ = Target(width, height);
    half_ = Target(width / 2, height / 2);
    quarter_ = Target(width / 4, height / 4);
    blurA_ = Target(width / 4, height / 4);
    blurB_ = Target(width / 4, height / 4);
    eighth_ = Target(width / 8, height / 8);
    sixteenth_ = Target(width / 16, height / 16);
    width_ = width;
    height_ = height;
    targetsReady_ = true;
}

void Backdrop::Capture(int width, int height, const std::function<void()>& draw)
{
    if (width <= 0 || height <= 0)
        return;
    EnsureTargets(width, height);
    BeginTextureMode(scene_);
    ClearBackground(BLACK);
    draw();
    EndTextureMode();

    // Halved twice: bilinear filtering at the middle of each two-by-two block is their
    // average, which a single jump to a quarter would not be.
    DrawInto(scene_.texture, half_);
    DrawInto(half_.texture, quarter_);
    if (!shaderLoaded_)
    {
        DrawInto(quarter_.texture, eighth_);
        DrawInto(eighth_.texture, sixteenth_);
        Perf::Count(Perf::Counter::Passes, 4);
        return;
    }
    // Bloom's blur with nothing picked out: nine taps, a texel and a half apart at a quarter
    // of the screen, which is about the fourteen pixels a frosted pane blurs by.
    const float plain = 0.0f;
    const float across[2] = { 1.5f / (float)quarter_.texture.width, 0.0f };
    const float down[2] = { 0.0f, 1.5f / (float)quarter_.texture.height };
    SetShaderValue(blur_, locBrightPass_, &plain, SHADER_UNIFORM_FLOAT);
    SetShaderValue(blur_, locDirection_, across, SHADER_UNIFORM_VEC2);
    BeginShaderMode(blur_);
    DrawInto(quarter_.texture, blurA_);
    EndShaderMode();
    SetShaderValue(blur_, locDirection_, down, SHADER_UNIFORM_VEC2);
    BeginShaderMode(blur_);
    DrawInto(blurA_.texture, blurB_);
    EndShaderMode();
    Perf::Count(Perf::Counter::Passes, 4);
}

void Backdrop::Present() const
{
    if (!targetsReady_)
        return;
    DrawTexturePro(scene_.texture, { 0.0f, 0.0f, (float)width_, -(float)height_ },
                   { 0.0f, 0.0f, (float)width_, (float)height_ }, { 0.0f, 0.0f }, 0.0f, WHITE);
}

const Texture2D& Backdrop::Blurred() const
{
    return shaderLoaded_ ? blurB_.texture : sixteenth_.texture;
}

}  // namespace Render
