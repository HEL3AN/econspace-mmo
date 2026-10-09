#include "render/CameraRig.h"

#include <cmath>

namespace Render
{
namespace
{
float Clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

// The fraction of the remaining distance to cover this frame, so a glide takes the same
// time at any frame rate. A fixed per-frame fraction would be twice as fast at 120 Hz.
float Ease(float rate, float dt)
{
    return 1.0f - std::exp(-rate * dt);
}

Vector2 ScreenToWorld(const Camera2D& c, Vector2 s)
{
    return { c.target.x + (s.x - c.offset.x) / c.zoom, c.target.y + (s.y - c.offset.y) / c.zoom };
}
}  // namespace

void CameraRig::SetViewport(float width, float height)
{
    camera_.offset = { width * 0.5f, height * 0.5f };
}

void CameraRig::Zoom(float steps, Vector2 anchorScreen)
{
    if (steps == 0.0f)
        return;
    // Multiplicative, so one notch of the wheel is the same proportion at every scale --
    // a step that moves a hull view noticeably would otherwise do nothing to a system view.
    zoomGoal_ = Clampf(zoomGoal_ * std::pow(1.18f, steps), MIN_ZOOM, MAX_ZOOM);

    if (following_)
    {
        anchored_ = false;
        return;
    }
    anchored_ = true;
    anchorScreen_ = anchorScreen;
    anchorWorld_ = ScreenToWorld(camera_, anchorScreen);
}

void CameraRig::Pan(Vector2 screenDelta)
{
    if (screenDelta.x == 0.0f && screenDelta.y == 0.0f)
        return;
    following_ = false;
    anchored_ = false;
    camera_.target.x -= screenDelta.x / camera_.zoom;
    camera_.target.y -= screenDelta.y / camera_.zoom;
}

void CameraRig::Recenter()
{
    following_ = true;
    anchored_ = false;
}

void CameraRig::Snap(Vector2 shipPos)
{
    following_ = true;
    anchored_ = false;
    camera_.target = shipPos;
    zoomNow_ = zoomGoal_;
    camera_.zoom = zoomNow_ * warpPull_;
}

void CameraRig::SetZoom(float zoom)
{
    zoomGoal_ = zoomNow_ = std::clamp(zoom, MIN_ZOOM, MAX_ZOOM);
    camera_.zoom = zoomNow_ * warpPull_;
}

void CameraRig::Update(float dt, Vector2 shipPos, bool warping)
{
    // The zoom glides in proportion rather than in absolute steps, for the same reason the
    // wheel is multiplicative: the distance from 0.001 to 0.002 and from 1 to 2 should take
    // the same time.
    zoomNow_ *= std::pow(zoomGoal_ / zoomNow_, Ease(12.0f, dt));

    // Pulled back in warp, so the speed reads as speed rather than as a ship sitting still
    // while the background flickers. Eased both ways, slower on the way back so the arrival
    // is something you see.
    warpPull_ += ((warping ? WARP_PULL : 1.0f) - warpPull_) * Ease(warping ? 3.0f : 1.6f, dt);
    camera_.zoom = Clampf(zoomNow_ * warpPull_, MIN_ZOOM, MAX_ZOOM);

    if (following_)
    {
        // In warp the ship is too fast to be chased smoothly; held exactly, or it leaves
        // the screen. Otherwise a short glide, which is what makes a turn feel like weight.
        if (warping)
            camera_.target = shipPos;
        else
        {
            const float f = Ease(8.0f, dt);
            camera_.target.x += (shipPos.x - camera_.target.x) * f;
            camera_.target.y += (shipPos.y - camera_.target.y) * f;
        }
        return;
    }

    // Looking away: keep the world point the cursor was over when the wheel turned under
    // the cursor while the zoom glides, the way a map behaves.
    if (anchored_)
    {
        camera_.target.x = anchorWorld_.x - (anchorScreen_.x - camera_.offset.x) / camera_.zoom;
        camera_.target.y = anchorWorld_.y - (anchorScreen_.y - camera_.offset.y) / camera_.zoom;
        if (std::fabs(zoomNow_ / zoomGoal_ - 1.0f) < 0.001f)
            anchored_ = false;
    }
}

ScaleBar ScaleBarFor(float zoom, float maxPixels)
{
    ScaleBar bar;
    if (zoom <= 0.0f || maxPixels <= 0.0f)
        return bar;

    // The longest of 1, 2 or 5 times a power of ten that still fits. Round numbers, because
    // a bar labelled 3,742 tells a player nothing they can hold in their head.
    const float maxWorld = maxPixels / zoom;
    float       power = std::pow(10.0f, std::floor(std::log10(maxWorld)));
    const float steps[] = { 5.0f, 2.0f, 1.0f };
    for (float s : steps)
        if (s * power <= maxWorld)
        {
            bar.worldLength = s * power;
            break;
        }
    if (bar.worldLength == 0.0f)
        bar.worldLength = power;  // unreachable for a valid zoom, but never zero
    bar.pixels = bar.worldLength * zoom;
    return bar;
}

}  // namespace Render
