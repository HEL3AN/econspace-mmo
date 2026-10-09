#pragma once

#include "raylib.h"

namespace Render
{

// The camera belongs to the player, not to the ship (#158).
//
// It used to be the ship's: locked on it, one zoom band, and the only way to see anything
// further away was to scroll out until it came on screen. That was clumsy at twenty-five
// thousand units and is impossible at the million a system is about to become (#159).
// The player chooses where to look; the ship is where the camera *returns* to.
//
// This class takes its input as arguments rather than reading the keyboard, so it can be
// tested without a window -- the same split that kept the rest of M6 testable.
class CameraRig
{
public:
    // From a hull filling the screen to a whole system on it. The far end is set by the
    // system a million units across (#159) on a 720-pixel window, with room to spare.
    static constexpr float MIN_ZOOM = 0.00025f;
    static constexpr float MAX_ZOOM = 4.0f;

    // How far back the camera pulls while the ship is in warp. A multiplier on the zoom
    // the player chose, not a replacement for it: when the warp ends they are back where
    // they were.
    static constexpr float WARP_PULL = 0.35f;

    void SetViewport(float width, float height);

    // The wheel. While the camera is on the ship it zooms about the ship, or following would
    // drag it off centre; once the player has looked away it zooms about the cursor, the way
    // a map does, so the thing under the pointer stays under the pointer.
    void Zoom(float steps, Vector2 anchorScreen);

    // A drag, in screen pixels. Looking away detaches the camera from the ship.
    void Pan(Vector2 screenDelta);

    // Back to the ship, gliding rather than jumping.
    void Recenter();

    // A change of system: no glide, because there is nothing between here and there to see.
    void Snap(Vector2 shipPos);

    void Update(float dt, Vector2 shipPos, bool warping);

    bool            Following() const { return following_; }
    float           ZoomLevel() const { return camera_.zoom; }
    const Camera2D& Camera() const { return camera_; }

private:
    Camera2D camera_{ { 0.0f, 0.0f }, { 0.0f, 0.0f }, 0.0f, 1.0f };
    float    zoomGoal_ = 1.0f;  // what the player asked for
    float    zoomNow_ = 1.0f;   // where the glide toward it has got to
    float    warpPull_ = 1.0f;  // eases toward WARP_PULL in warp and back to 1 after
    bool     following_ = true;

    // While zooming about the cursor: the world point that has to stay under it.
    bool    anchored_ = false;
    Vector2 anchorWorld_ = { 0.0f, 0.0f };
    Vector2 anchorScreen_ = { 0.0f, 0.0f };
};

// A scale bar: a round length of world, and how many pixels it is at this zoom. In a system
// a million units across, a player cannot tell what they are looking at without one.
struct ScaleBar
{
    float worldLength = 0.0f;  // 1, 2 or 5 times a power of ten
    float pixels = 0.0f;
};

// The longest round length that fits in `maxPixels` at `zoom`.
ScaleBar ScaleBarFor(float zoom, float maxPixels);

}  // namespace Render
