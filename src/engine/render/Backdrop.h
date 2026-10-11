#pragma once

#include "raylib.h"
#include <functional>
#include <string>

namespace Render
{

// The world blurred once a frame, for glass windows to stand on (#297, "Blur behind
// windows"). Blurring under each window would cost a pass per window; this draws the world
// into a texture once, shrinks it to a quarter in two halvings (each an average of four
// pixels, so a star does not flicker in and out of the sample), and blurs that across and
// down with bloom's own blur shader. Every window then samples the same small texture under
// itself (Ui::Backdrop), which costs no more than drawing its panel.
//
// A shader failing is a runtime fact (#120): without the blur shader the shrinking goes on
// to a sixteenth, which bilinear filtering brings back up as a coarser blur, and the
// setting still works.
class Backdrop
{
public:
    ~Backdrop();

    // The blur shader, from `<dataDir>/shaders/bloom_blur.fs`. After InitWindow.
    void Load(const std::string& dataDir);
    void Unload();

    // Draws the world (`draw`) into a texture the size of the screen and blurs it. Outside
    // BeginDrawing and any texture mode: it uses targets of its own.
    void Capture(int width, int height, const std::function<void()>& draw);
    // The world as it was captured, full size, into whatever is being drawn to: the frame
    // draws this instead of drawing the world twice.
    void Present() const;

    const Texture2D& Blurred() const;
    bool             ShaderLoaded() const { return shaderLoaded_; }

private:
    void EnsureTargets(int width, int height);
    void FreeTargets();

    RenderTexture2D scene_{};    // the world, full size
    RenderTexture2D half_{};     // ...a half
    RenderTexture2D quarter_{};  // ...a quarter: what the blur reads
    RenderTexture2D blurA_{};    // across
    RenderTexture2D blurB_{};    // ...and down: what the windows sample
    RenderTexture2D eighth_{};   // without the shader: shrunk further instead
    RenderTexture2D sixteenth_{};
    bool            targetsReady_ = false;
    int             width_ = 0, height_ = 0;

    Shader blur_{};
    bool   shaderLoaded_ = false;
    int    locDirection_ = -1;
    int    locBrightPass_ = -1;
};

}  // namespace Render
