#include <doctest/doctest.h>

#include "render/CameraRig.h"
#include <cmath>
#include <initializer_list>

// The camera takes its input as arguments, so how it behaves can be tested without a window
// (#158). Everything here is the part that would otherwise be judged only by flying around.

namespace
{
Render::CameraRig Rig()
{
    Render::CameraRig r;
    r.SetViewport(1280.0f, 720.0f);
    r.Snap({ 0.0f, 0.0f });
    return r;
}

void Run(Render::CameraRig& r, float seconds, Vector2 ship, bool warping = false)
{
    for (float t = 0.0f; t < seconds; t += 1.0f / 60.0f)
        r.Update(1.0f / 60.0f, ship, warping);
}

Vector2 ToScreen(const Camera2D& c, Vector2 w)
{
    return { (w.x - c.target.x) * c.zoom + c.offset.x, (w.y - c.target.y) * c.zoom + c.offset.y };
}
}  // namespace

TEST_CASE("the camera follows the ship until the player looks away")
{
    Render::CameraRig r = Rig();
    Run(r, 2.0f, { 500.0f, 0.0f });
    CHECK(r.Following());
    CHECK(r.Camera().target.x == doctest::Approx(500.0f).epsilon(0.01));

    SUBCASE("looking away stops it following, and the ship can leave the screen")
    {
        r.Pan({ 300.0f, 0.0f });
        CHECK_FALSE(r.Following());
        const float held = r.Camera().target.x;
        Run(r, 2.0f, { 5000.0f, 0.0f });
        CHECK(r.Camera().target.x == doctest::Approx(held));
    }

    SUBCASE("and it comes back when asked, gliding rather than jumping")
    {
        r.Pan({ 300.0f, 0.0f });
        r.Recenter();
        r.Update(1.0f / 60.0f, { 500.0f, 0.0f }, false);
        const float oneFrame = r.Camera().target.x;
        CHECK(std::fabs(oneFrame - 500.0f) > 1.0f);  // not there in one frame
        Run(r, 2.0f, { 500.0f, 0.0f });
        CHECK(r.Camera().target.x == doctest::Approx(500.0f).epsilon(0.01));
    }
}

TEST_CASE("zoom reaches from a hull to a whole system, the same proportion per notch")
{
    Render::CameraRig r = Rig();
    for (int i = 0; i < 200; i++)
        r.Zoom(-1.0f, { 640.0f, 360.0f });
    Run(r, 3.0f, { 0.0f, 0.0f });
    // A million-unit system (#159) fits on a 720-pixel window.
    CHECK(r.ZoomLevel() * 2.0f * 1000000.0f <= 720.0f);

    for (int i = 0; i < 400; i++)
        r.Zoom(1.0f, { 640.0f, 360.0f });
    Run(r, 3.0f, { 0.0f, 0.0f });
    // A sixteen-unit hull is large enough to see its parts.
    CHECK(r.ZoomLevel() * 16.0f >= 40.0f);
}

TEST_CASE("once the camera is free, zooming keeps the point under the cursor under the cursor")
{
    // The way a map behaves. While following the ship it zooms about the ship instead, or
    // following would drag it off centre.
    Render::CameraRig r = Rig();
    r.Pan({ 10.0f, 0.0f });  // looking away
    Run(r, 0.5f, { 0.0f, 0.0f });

    const Vector2  cursor{ 900.0f, 200.0f };
    const Camera2D before = r.Camera();
    const Vector2  underCursor{ before.target.x + (cursor.x - before.offset.x) / before.zoom,
                                before.target.y + (cursor.y - before.offset.y) / before.zoom };

    r.Zoom(3.0f, cursor);
    Run(r, 2.0f, { 0.0f, 0.0f });
    const Vector2 after = ToScreen(r.Camera(), underCursor);
    CHECK(after.x == doctest::Approx(cursor.x).epsilon(0.01));
    CHECK(after.y == doctest::Approx(cursor.y).epsilon(0.01));
    CHECK(r.ZoomLevel() > before.zoom);
}

TEST_CASE("warp pulls the camera back without losing the zoom the player chose")
{
    Render::CameraRig r = Rig();
    Run(r, 1.0f, { 0.0f, 0.0f });
    const float chosen = r.ZoomLevel();

    Run(r, 3.0f, { 0.0f, 0.0f }, true);
    CHECK(r.ZoomLevel() < chosen * 0.5f);

    Run(r, 6.0f, { 0.0f, 0.0f }, false);
    CHECK(r.ZoomLevel() == doctest::Approx(chosen).epsilon(0.02));

    SUBCASE("and in warp the ship is held exactly, or it would leave the screen")
    {
        r.Update(1.0f / 60.0f, { 80000.0f, 0.0f }, true);
        CHECK(r.Camera().target.x == doctest::Approx(80000.0f));
    }
}

TEST_CASE("the scale bar is a round length that fits")
{
    // Round numbers, because a bar labelled 3,742 tells a player nothing they can hold.
    for (float zoom : { 4.0f, 1.0f, 0.37f, 0.01f, 0.0003f })
    {
        const Render::ScaleBar b = Render::ScaleBarFor(zoom, 160.0f);
        INFO("zoom ", zoom);
        CHECK(b.pixels <= 160.0f + 0.01f);
        CHECK(b.pixels > 160.0f / 5.0f);  // never uselessly short: 1-2-5 steps are at most 2.5x
        const float mantissa =
            b.worldLength / std::pow(10.0f, std::floor(std::log10(b.worldLength)));
        CHECK((std::fabs(mantissa - 1.0f) < 0.01f || std::fabs(mantissa - 2.0f) < 0.01f ||
               std::fabs(mantissa - 5.0f) < 0.01f));
    }
}
