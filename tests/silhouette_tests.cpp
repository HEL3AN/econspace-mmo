#include <doctest/doctest.h>

#include "core/Archetype.h"
#include "render/Modules.h"
#include "render/Silhouette.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <raylib.h>
#include <set>
#include <nlohmann/json.hpp>
#include <string>

// A shape is a composition, and where it ends up is geometry: repeats turned about the
// centre, the object's own heading applied, fractions of a radius turned into world units,
// detail dropped when it would be invisible, and jitter that is different per part but the
// same every frame. None of that needs a graphics card, and all of it is where a
// composition silently comes out wrong.

namespace
{
Render::Shape Parse(const char* text)
{
    Render::Shape s;
    std::string   error;
    REQUIRE(Render::ParseShape(nlohmann::json::parse(text), s, error));
    CHECK(error.empty());
    return s;
}

float Dist(Vector2 a, Vector2 b)
{
    return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y));
}

// The pose these tests care about: where, how big, which way, whose seed, at what zoom.
// The clock and the engine are separate because only the motion cases touch them.
Render::Pose At(Vector2 pos, float size, float heading, int seed, float zoom)
{
    Render::Pose p;
    p.pos = pos;
    p.size = size;
    p.heading = heading;
    p.seed = seed;
    p.pixelsPerUnit = zoom;
    return p;
}
}  // namespace

TEST_CASE("every form and role is named the same way in data and in code")
{
    const char* forms[] = { "disc", "ring", "polygon", "capsule", "chevron", "bar", "lattice" };
    for (const char* n : forms)
    {
        Render::Form f;
        INFO("form: ", n);
        REQUIRE(Render::FormFromName(n, f));
        CHECK(std::string(Render::FormName(f)) == n);
    }

    const char* roles[] = { "hull", "panel", "trim", "light", "antenna" };
    for (const char* n : roles)
    {
        Render::Role r;
        INFO("role: ", n);
        REQUIRE(Render::RoleFromName(n, r));
        CHECK(std::string(Render::RoleName(r)) == n);
    }

    Render::Form unused;
    CHECK_FALSE(Render::FormFromName("blob", unused));
}

TEST_CASE("a part that is not understood is refused, and says which one")
{
    // Deliberately unlike a screen-treatment pass, which is skipped. A part that quietly
    // vanishes leaves an object missing a piece, and nobody would know which file to open.
    Render::Shape s;
    std::string   error;
    CHECK_FALSE(Render::ParseShape(nlohmann::json::parse(R"([{ "form": "blob" }])"), s, error));
    CHECK(error.find("blob") != std::string::npos);

    CHECK_FALSE(Render::ParseShape(nlohmann::json::parse(R"([{ "role": "greeble" }])"), s, error));
    CHECK(error.find("greeble") != std::string::npos);

    CHECK_FALSE(Render::ParseShape(nlohmann::json::parse(R"({ "form": "disc" })"), s, error));
}

TEST_CASE("a shape is written in fractions and comes out in world units")
{
    const Render::Shape s = Parse(R"([
        { "form": "polygon", "sides": 6, "radius": 1.0 },
        { "form": "disc", "at": [0.8, 0.0], "radius": 0.2 }
    ])");

    const std::vector<Render::Piece> p =
        Render::Compose(s, At({ 100.0f, 50.0f }, 90.0f, 0.0f, 1, 1.0f));
    REQUIRE(p.size() == 2);

    CHECK(p[0].radius == doctest::Approx(90.0f));  // one radius
    CHECK(p[0].pos.x == doctest::Approx(100.0f));
    CHECK(p[1].radius == doctest::Approx(18.0f));          // a fifth of one
    CHECK(p[1].pos.x == doctest::Approx(100.0f + 72.0f));  // 0.8 of the radius out
    CHECK(p[1].pos.y == doctest::Approx(50.0f));
}

TEST_CASE("a repeated part is turned about the centre, not copied on top of itself")
{
    const Render::Shape s = Parse(R"([
        { "form": "capsule", "at": [1.0, 0.0], "repeat": 3, "length": 0.5, "width": 0.2 }
    ])");

    const std::vector<Render::Piece> p =
        Render::Compose(s, At({ 0.0f, 0.0f }, 100.0f, 0.0f, 1, 1.0f));
    REQUIRE(p.size() == 3);

    // All the same distance out, none in the same place, a third of a turn apart.
    for (const Render::Piece& piece : p)
        CHECK(Dist(piece.pos, { 0.0f, 0.0f }) == doctest::Approx(100.0f));
    CHECK(Dist(p[0].pos, p[1].pos) > 1.0f);
    CHECK(Dist(p[1].pos, p[2].pos) > 1.0f);
    CHECK(p[1].angle - p[0].angle == doctest::Approx(120.0f));
}

TEST_CASE("the object's heading turns the whole composition, so a ship's parts follow its nose")
{
    const Render::Shape s = Parse(R"([{ "form": "chevron", "at": [1.0, 0.0], "length": 0.4 }])");

    const std::vector<Render::Piece> ahead =
        Render::Compose(s, At({ 0.0f, 0.0f }, 10.0f, 0.0f, 1, 1.0f));
    const std::vector<Render::Piece> turned =
        Render::Compose(s, At({ 0.0f, 0.0f }, 10.0f, (float)M_PI / 2.0f, 1, 1.0f));

    REQUIRE(ahead.size() == 1);
    REQUIRE(turned.size() == 1);
    CHECK(ahead[0].pos.x == doctest::Approx(10.0f));
    CHECK(ahead[0].pos.y == doctest::Approx(0.0f).epsilon(0.01));
    // A quarter turn: the part that was ahead is now to the side, and it is pointing there.
    CHECK(turned[0].pos.x == doctest::Approx(0.0f).epsilon(0.01));
    CHECK(turned[0].pos.y == doctest::Approx(10.0f));
    CHECK(turned[0].angle == doctest::Approx(90.0f).epsilon(0.01));
}

TEST_CASE("detail appears with distance, and disappearing is the point")
{
    const Render::Shape s = Parse(R"([
        { "form": "polygon", "sides": 6, "radius": 1.0 },
        { "form": "disc", "radius": 0.1, "repeat": 6, "at": [1.2, 0.0], "minPixels": 60 }
    ])");

    // A ninety-unit station on the system map, a few pixels across: the core only. A
    // hundred parts resolved into eight pixels is a smudge, not a small object.
    CHECK(Render::Compose(s, At({ 0.0f, 0.0f }, 90.0f, 0.0f, 1, 0.03f)).size() == 1);

    // The same station up close: the lamps arrive.
    CHECK(Render::Compose(s, At({ 0.0f, 0.0f }, 90.0f, 0.0f, 1, 1.0f)).size() == 7);
}

TEST_CASE("two of the same thing differ, and neither of them shimmers")
{
    const Render::Shape s = Parse(R"([
        { "form": "capsule", "at": [1.0, 0.0], "repeat": 3, "length": 0.5,
          "jitterAngle": 6.0, "jitterScale": 0.1 }
    ])");

    const std::vector<Render::Piece> a = Render::Compose(s, At({ 0, 0 }, 100.0f, 0.0f, 7, 1.0f));
    const std::vector<Render::Piece> b = Render::Compose(s, At({ 0, 0 }, 100.0f, 0.0f, 8, 1.0f));
    REQUIRE(a.size() == 3);
    REQUIRE(b.size() == 3);

    SUBCASE("a different object is arranged differently")
    {
        bool differs = false;
        for (size_t i = 0; i < a.size(); i++)
            differs = differs || Dist(a[i].pos, b[i].pos) > 0.01f;
        CHECK(differs);
    }

    SUBCASE("the same object is arranged the same way every time it is asked")
    {
        // Jitter that changes between frames is not variation, it is a shimmer.
        const std::vector<Render::Piece> again =
            Render::Compose(s, At({ 0, 0 }, 100.0f, 0.0f, 7, 1.0f));
        for (size_t i = 0; i < a.size(); i++)
        {
            CHECK(again[i].pos.x == doctest::Approx(a[i].pos.x));
            CHECK(again[i].angle == doctest::Approx(a[i].angle));
        }
    }

    SUBCASE("the three repeats are perturbed differently, or the object is merely rotated")
    {
        const float d0 = a[0].angle - 0.0f;
        const float d1 = a[1].angle - 120.0f;
        const float d2 = a[2].angle - 240.0f;
        CHECK(std::fabs(d0 - d1) > 0.001f);
        CHECK(std::fabs(d1 - d2) > 0.001f);
    }

    SUBCASE("and it stays inside what the shape allowed")
    {
        for (const Render::Piece& piece : a)
            CHECK(piece.length <= doctest::Approx(0.5f * 100.0f * 1.1f));
    }
}

TEST_CASE("motion is a function of the clock and the seed, never of anything accumulated")
{
    // The whole reason it is written this way: every client computes the same answer from
    // the same time without a byte on the wire. A part whose angle were integrated per
    // frame would drift, and two players would see the same station turned differently.
    const Render::Shape ring = Parse(R"([{ "form": "ring", "radius": 1.0, "spin": 90.0 }])");

    Render::Pose p = At({ 0, 0 }, 100.0f, 0.0f, 1, 1.0f);
    p.time = 1.0f;
    const float atOne = Render::Compose(ring, p)[0].angle;
    p.time = 2.0f;
    const float atTwo = Render::Compose(ring, p)[0].angle;
    CHECK(atTwo - atOne == doctest::Approx(90.0f));  // ninety degrees a second

    SUBCASE("the same moment always gives the same answer")
    {
        p.time = 1.0f;
        CHECK(Render::Compose(ring, p)[0].angle == doctest::Approx(atOne));
    }

    SUBCASE("and it is still finite after a week of uptime")
    {
        // A float that has been counting degrees for a week has no precision left, so the
        // turn is wrapped rather than accumulated.
        p.time = 7.0f * 24.0f * 3600.0f;
        CHECK(std::fabs(Render::Compose(ring, p)[0].angle) <= 360.0f);
    }
}

TEST_CASE("a light blinks on its own phase, so a row of them is a sequence and not a pulse")
{
    const Render::Shape lamps = Parse(R"([
        { "form": "disc", "at": [1.0, 0.0], "repeat": 6, "radius": 0.05,
          "role": "light", "blink": 2.0 }
    ])");

    Render::Pose p = At({ 0, 0 }, 100.0f, 0.0f, 3, 1.0f);
    p.time = 0.4f;
    const std::vector<Render::Piece> lit = Render::Compose(lamps, p);
    REQUIRE(lit.size() == 6);

    bool differ = false;
    for (size_t i = 1; i < lit.size(); i++)
        differ = differ || std::fabs(lit[i].brightness - lit[0].brightness) > 0.01f;
    CHECK(differ);  // in step they would read as a screensaver

    SUBCASE("a part that does not blink is simply at full")
    {
        const Render::Shape steady = Parse(R"([{ "form": "disc", "radius": 1.0 }])");
        CHECK(Render::Compose(steady, p)[0].brightness == doctest::Approx(1.0f));
    }

    SUBCASE("and a blink never goes out entirely")
    {
        // A lamp that reaches zero reads as a part that has fallen off.
        for (float t = 0.0f; t < 4.0f; t += 0.13f)
        {
            p.time = t;
            for (const Render::Piece& piece : Render::Compose(lamps, p))
                CHECK(piece.brightness > 0.2f);
        }
    }
}

TEST_CASE("a part can belong to the engine, and is absent when the engine is not burning")
{
    const Render::Shape s = Parse(R"([
        { "form": "chevron", "length": 2.0, "width": 1.0 },
        { "form": "chevron", "at": [-1.2, 0.0], "angle": 180, "length": 0.8, "width": 0.4,
          "role": "light", "onlyThrusting": true }
    ])");

    Render::Pose p = At({ 0, 0 }, 16.0f, 0.0f, 1, 1.0f);
    p.thrusting = false;
    CHECK(Render::Compose(s, p).size() == 1);
    p.thrusting = true;
    CHECK(Render::Compose(s, p).size() == 2);
}

TEST_CASE("an orbiting part goes round the body, behind it and in front of it")
{
    // Found by looking (#161): once a planet's surface discs started moving they read as
    // moons. Making them moons is better than what was intended, and the whole of the
    // depth effect is draw order.
    const Render::Shape s = Parse(R"([
        { "form": "disc", "radius": 1.0 },
        { "form": "disc", "radius": 0.1, "orbitRadius": 1.6, "orbitPeriod": 40.0,
          "orbitTilt": 0.3 }
    ])");

    Render::Pose p = At({ 0, 0 }, 100.0f, 0.0f, 5, 1.0f);

    // A whole lap, sampled. It has to go both in front of and behind the body, or it is a
    // ring rather than an orbit.
    bool behind = false, front = false;
    for (int i = 0; i < 40; i++)
    {
        p.time = (float)i;
        for (const Render::Piece& piece : Render::Compose(s, p))
        {
            if (piece.depth < -0.5f)
                behind = true;
            if (piece.depth > 0.5f)
                front = true;
        }
    }
    CHECK(behind);
    CHECK(front);

    SUBCASE("and what is behind is drawn before the body, so the body hides it")
    {
        // The entire trick. Compose returns pieces back to front; a backend draws them in
        // order and never has to know why.
        for (int i = 0; i < 40; i++)
        {
            p.time = (float)i;
            const std::vector<Render::Piece> pieces = Render::Compose(s, p);
            REQUIRE(pieces.size() == 2);
            CHECK(pieces[0].depth <= pieces[1].depth);
        }
    }

    SUBCASE("it stays on its ellipse, flattened by the tilt")
    {
        float maxAcross = 0.0f, maxUpDown = 0.0f;
        for (int i = 0; i < 80; i++)
        {
            p.time = (float)i * 0.5f;
            for (const Render::Piece& piece : Render::Compose(s, p))
                if (piece.radius < 50.0f)  // the moon, not the body
                {
                    maxAcross = std::fmax(maxAcross, std::fabs(piece.pos.x));
                    maxUpDown = std::fmax(maxUpDown, std::fabs(piece.pos.y));
                }
        }
        CHECK(maxAcross == doctest::Approx(160.0f).epsilon(0.1));
        // Squashed by (1 - tilt): an orbit seen edge-on is a line, and one seen from above
        // never passes behind anything.
        CHECK(maxUpDown == doctest::Approx(160.0f * 0.7f).epsilon(0.15));
    }

    SUBCASE("nearer is bigger, which is what stops it reading as a sprite under a circle")
    {
        float nearR = 0.0f, farR = 1e9f;
        for (int i = 0; i < 80; i++)
        {
            p.time = (float)i * 0.5f;
            for (const Render::Piece& piece : Render::Compose(s, p))
                if (piece.radius < 50.0f)
                {
                    nearR = std::fmax(nearR, piece.radius);
                    farR = std::fmin(farR, piece.radius);
                }
        }
        CHECK(nearR > farR);
    }

    SUBCASE("several of them are spread around the lap rather than stacked")
    {
        const Render::Shape many = Parse(R"([
            { "form": "disc", "radius": 1.0 },
            { "form": "disc", "radius": 0.08, "repeat": 3, "orbitRadius": 1.5 }
        ])");
        p.time = 3.0f;
        const std::vector<Render::Piece> pieces = Render::Compose(many, p);
        REQUIRE(pieces.size() == 4);
        // No two moons in the same place; three moons on top of each other is one moon.
        for (size_t i = 0; i < pieces.size(); i++)
            for (size_t j = i + 1; j < pieces.size(); j++)
                if (pieces[i].radius < 50.0f && pieces[j].radius < 50.0f)
                    CHECK(Dist(pieces[i].pos, pieces[j].pos) > 1.0f);
    }
}

TEST_CASE("a surface feature lives on the sphere: across the face, round the back, foreshortened")
{
    // #166. A planet turns about an axis lying nearly in the picture, so its features
    // travel *across* the disc and round the back -- not about the disc's centre like a
    // wheel, which is what they did before and is not what a planet does.
    const Render::Shape s = Parse(R"({ "tilt": 0, "parts": [
        { "form": "disc", "radius": 1.0 },
        { "form": "disc", "lat": 0, "lon": 0, "radius": 0.1, "spin": 10.0 }
    ]})");

    Render::Pose p = At({ 0, 0 }, 100.0f, 0.0f, 1, 1.0f);

    // A whole turn, sampled. Visible for roughly half of it and gone for the rest.
    int seen = 0, hidden = 0;
    for (int i = 0; i < 36; i++)
    {
        p.time = (float)i;  // 10 degrees a second, so 36 samples are one turn
        const std::vector<Render::Piece> pieces = Render::Compose(s, p);
        if (pieces.size() == 2)
            seen++;
        else
            hidden++;
    }
    CHECK(seen > 10);
    CHECK(hidden > 10);

    SUBCASE("it stays on the disc and moves across it, not round it")
    {
        for (int i = 0; i < 36; i++)
        {
            p.time = (float)i;
            for (const Render::Piece& piece : Render::Compose(s, p))
                if (piece.surface)
                {
                    CHECK(Dist(piece.pos, { 0, 0 }) <= 100.0f + 0.01f);
                    // On the equator with no tilt: it slides along the horizontal diameter.
                    CHECK(std::fabs(piece.pos.y) < 0.01f);
                }
        }
    }

    SUBCASE("facing the viewer it is round; near the limb it is an ellipse")
    {
        float mostSquashed = 1.0f, leastSquashed = 0.0f;
        for (int i = 0; i < 72; i++)
        {
            p.time = (float)i * 0.5f;
            for (const Render::Piece& piece : Render::Compose(s, p))
                if (piece.surface)
                {
                    mostSquashed = std::fmin(mostSquashed, piece.squash);
                    leastSquashed = std::fmax(leastSquashed, piece.squash);
                }
        }
        CHECK(leastSquashed > 0.95f);
        CHECK(mostSquashed < 0.4f);
    }

    SUBCASE("it is lit as the body it is part of, not as a small planet of its own")
    {
        p.time = 0.0f;
        for (const Render::Piece& piece : Render::Compose(s, p))
            if (piece.surface)
            {
                CHECK(piece.bodyRadius == doctest::Approx(100.0f));
                CHECK(piece.bodyPos.x == doctest::Approx(0.0f));
            }
    }
}

TEST_CASE("a surface feature ends at the limb: its outline is cut to the body's disc (#240)")
{
    // A large storm near the limb is still a whole ellipse, and drawn as one it bulged past
    // the planet's edge. Soft or solid, what is drawn is its outline cut to the body.
    for (const std::string soft : { "false", "true" })
    {
        INFO("soft: ", soft);
        const std::string   text = std::string(R"({ "tilt": 15, "parts": [
            { "form": "disc", "radius": 1.0 },
            { "form": "disc", "lat": 20, "lon": 0, "radius": 0.45, "spin": 10.0, "soft": )") +
                                   soft + "} ]}";
        const Render::Shape s = Parse(text.c_str());
        Render::Pose        p = At({ 50.0f, -20.0f }, 100.0f, 0.3f, 1, 1.0f);

        bool cut = false, whole = false, seen = false;
        for (int i = 0; i < 360; i++)
        {
            p.time = (float)i * 0.1f;
            for (const Render::Piece& piece : Render::Compose(s, p))
            {
                if (!piece.surface)
                    continue;
                seen = true;
                CHECK(piece.soft == (soft == "true"));
                const std::vector<Vector2> outline = Render::SurfaceOutline(piece);
                REQUIRE(outline.size() >= 3);
                const Vector2 from = Render::SurfaceFanCentre(piece, outline);
                if (Dist(piece.pos, piece.bodyPos) < 99.0f)
                    CHECK(Dist(from, piece.pos) < 1e-4f);  // a glow is brightest at its centre
                bool allOnRim = true;
                for (size_t k = 0; k < outline.size(); k++)
                {
                    // Inside the body, and inside the very polygon it is drawn as.
                    CHECK(Dist(outline[k], piece.bodyPos) <= 100.0f + 0.01f);
                    allOnRim = allOnRim && Render::EllipseReach(piece, outline[k]) > 0.999f;
                    // Convex around where it is fanned from, so the fan covers it.
                    const Vector2 a = outline[k], b = outline[(k + 1) % outline.size()];
                    const Vector2 a0 = outline[0], b0 = outline[1];
                    const float   cross =
                        (a.x - from.x) * (b.y - from.y) - (a.y - from.y) * (b.x - from.x);
                    const float cross0 =
                        (a0.x - from.x) * (b0.y - from.y) - (a0.y - from.y) * (b0.x - from.x);
                    CHECK(cross * cross0 >= -1e-3f);
                }
                (allOnRim ? whole : cut) = true;
            }
        }
        CHECK(seen);
        CHECK(whole);  // facing the viewer, nothing to cut
        CHECK(cut);    // at the limb, cut
    }

    SUBCASE("a soft part on the sphere is foreshortened and hidden round the back like any")
    {
        const Render::Shape s = Parse(R"({ "tilt": 0, "parts": [
            { "form": "disc", "radius": 1.0 },
            { "form": "disc", "lat": 0, "lon": 0, "radius": 0.2, "spin": 10.0, "soft": true }
        ]})");
        Render::Pose        p = At({ 0, 0 }, 100.0f, 0.0f, 1, 1.0f);
        int                 hidden = 0;
        float               mostSquashed = 1.0f;
        for (int i = 0; i < 72; i++)
        {
            p.time = (float)i * 0.5f;
            const std::vector<Render::Piece> pieces = Render::Compose(s, p);
            if (pieces.size() == 1)
                hidden++;
            for (const Render::Piece& piece : pieces)
                if (piece.soft)
                    mostSquashed = std::fmin(mostSquashed, piece.squash);
        }
        CHECK(hidden > 10);
        CHECK(mostSquashed < 0.4f);
    }

    SUBCASE("a round piece off the sphere has no surface outline")
    {
        Render::Piece off;
        off.radius = 10.0f;
        CHECK(Render::SurfaceOutline(off).empty());
    }
}

TEST_CASE("a point on a body faces the light as the hull material says it does")
{
    // Used where there is no shader, so a glow on a planet still goes dark on the night side.
    const Vector2 toRight{ 1.0f, 0.0f };
    const float   centre = Render::SurfaceFacing({ 0.0f, 0.0f }, toRight);
    const float   litLimb = Render::SurfaceFacing({ 0.95f, 0.0f }, toRight);
    const float   darkLimb = Render::SurfaceFacing({ -0.95f, 0.0f }, toRight);
    CHECK(centre > 0.2f);
    CHECK(centre < 1.0f);
    CHECK(litLimb > centre);
    CHECK(darkLimb == doctest::Approx(0.0f));
    // No direction at all is lit from everywhere, not from nowhere (#119).
    CHECK(Render::SurfaceFacing({ -0.95f, 0.0f }, { 0.0f, 0.0f }) == doctest::Approx(1.0f));
}

TEST_CASE("a latitude band is projected, so it narrows to the poles and never leaves the body")
{
    // A band drawn as a bar crosses the limb and reads as a stripe painted on a circle. A
    // band projected from a latitude is widest at the equator, shrinks toward the poles on
    // its own, and is bounded by the limb because nothing is ever outside it.
    Render::Pose p = At({ 0, 0 }, 100.0f, 0.0f, 1, 1.0f);

    auto bandOf = [&](const char* text)
    {
        const Render::Shape s = Parse(text);
        for (const Render::Piece& piece : Render::Compose(s, p))
            if (piece.form == Render::Form::Band)
                return piece;
        return Render::Piece{};
    };

    const Render::Piece equator =
        bandOf(R"({ "tilt": 20, "parts": [{ "form": "band", "lat": 0, "width": 10 }] })");
    const Render::Piece high =
        bandOf(R"({ "tilt": 20, "parts": [{ "form": "band", "lat": 60, "width": 10 }] })");
    REQUIRE_FALSE(equator.strip.empty());
    REQUIRE_FALSE(high.strip.empty());

    float equatorWide = 0.0f, highWide = 0.0f;
    for (const Vector2& v : equator.strip)
    {
        CHECK(Dist(v, { 0, 0 }) <= 100.0f + 0.05f);  // inside the body
        equatorWide = std::fmax(equatorWide, std::fabs(v.x));
    }
    for (const Vector2& v : high.strip)
    {
        CHECK(Dist(v, { 0, 0 }) <= 100.0f + 0.05f);
        highWide = std::fmax(highWide, std::fabs(v.x));
    }
    CHECK(equatorWide == doctest::Approx(100.0f).epsilon(0.02));  // reaches the limb
    CHECK(highWide < equatorWide * 0.6f);  // a sixty-degree band is cos(60) as wide

    SUBCASE("north is up: a northern band sits above a southern one")
    {
        const Render::Piece south =
            bandOf(R"({ "tilt": 20, "parts": [{ "form": "band", "lat": -40, "width": 10 }] })");
        float highY = 0.0f, southY = 0.0f;
        for (const Vector2& v : high.strip)
            highY += v.y;
        for (const Vector2& v : south.strip)
            southY += v.y;
        CHECK(highY / (float)high.strip.size() < southY / (float)south.strip.size());
    }

    SUBCASE("tipped toward the viewer the band bows; edge-on it is a straight chord")
    {
        const Render::Piece flat =
            bandOf(R"({ "tilt": 0, "parts": [{ "form": "band", "lat": 0, "width": 10 }] })");
        float lo = 1e9f, hi = -1e9f;
        for (const Vector2& v : flat.strip)
        {
            lo = std::fmin(lo, v.y);
            hi = std::fmax(hi, v.y);
        }
        // Edge-on, a ten-degree band is a straight slab exactly 2*sin(5 deg) of the radius
        // tall, wherever along it you measure.
        CHECK(hi - lo == doctest::Approx(200.0f * std::sin(5.0f * DEG2RAD)).epsilon(0.01));

        float tlo = 1e9f, thi = -1e9f;
        for (const Vector2& v : equator.strip)
        {
            tlo = std::fmin(tlo, v.y);
            thi = std::fmax(thi, v.y);
        }
        CHECK(thi - tlo > hi - lo);  // the same band, tipped, spans more height: it curves
    }

    SUBCASE("a band does not move when the planet turns -- its storms do")
    {
        p.time = 0.0f;
        const Render::Piece a = bandOf(
            R"({ "tilt": 20, "parts": [{ "form": "band", "lat": 10, "width": 8, "spin": 30 }] })");
        p.time = 5.0f;
        const Render::Piece b = bandOf(
            R"({ "tilt": 20, "parts": [{ "form": "band", "lat": 10, "width": 8, "spin": 30 }] })");
        REQUIRE(a.strip.size() == b.strip.size());
        for (size_t k = 0; k < a.strip.size(); k++)
            CHECK(Dist(a.strip[k], b.strip[k]) < 0.001f);
    }

    SUBCASE("a polar cap tipped away is mostly hidden, tipped toward the viewer it is not")
    {
        const Render::Piece north =
            bandOf(R"({ "tilt": 25, "parts": [{ "form": "band", "lat": 80, "width": 20 }] })");
        const Render::Piece south =
            bandOf(R"({ "tilt": 25, "parts": [{ "form": "band", "lat": -80, "width": 20 }] })");
        CHECK(north.strip.size() > south.strip.size());
    }
}

TEST_CASE("a shape may carry properties of the body, and a bare list still works")
{
    Render::Shape s;
    std::string   error;
    REQUIRE(Render::ParseShape(
        nlohmann::json::parse(R"({ "tilt": 33, "parts": [{ "form": "disc" }] })"), s, error));
    CHECK(s.axisTilt == doctest::Approx(33.0f));
    CHECK(s.parts.size() == 1);

    REQUIRE(Render::ParseShape(nlohmann::json::parse(R"([{ "form": "disc" }])"), s, error));
    CHECK(s.parts.size() == 1);

    CHECK_FALSE(Render::ParseShape(nlohmann::json::parse(R"({ "tilt": 10 })"), s, error));
    CHECK(error.find("parts") != std::string::npos);
}

TEST_CASE("a composition reaches as far as its furthest part, not as far as its radius")
{
    // A shape is written around a radius of one but need not stay inside it. Anything
    // framing an object has to ask, or a docking ring is cut off by the card meant to
    // show it.
    CHECK(Render::Extent(Render::Shape{}) == doctest::Approx(1.0f));

    const Render::Shape ringed = Parse(R"([
        { "form": "polygon", "sides": 6, "radius": 0.55 },
        { "form": "ring", "radius": 1.55, "width": 0.08 }
    ])");
    CHECK(Render::Extent(ringed) == doctest::Approx(1.55f));

    const Render::Shape armed = Parse(R"([
        { "form": "capsule", "at": [1.0, 0.0], "length": 0.8, "repeat": 3 }
    ])");
    CHECK(Render::Extent(armed) == doctest::Approx(1.4f));  // out to the arm, plus half of it

    SUBCASE("and it allows for however far the jitter may push a part")
    {
        const Render::Shape wobbly =
            Parse(R"([{ "form": "disc", "at": [1.0, 0.0], "radius": 0.2, "jitterScale": 0.5 }])");
        CHECK(Render::Extent(wobbly) > 1.2f);
    }
}

TEST_CASE("a part is shaded at its own cross section, not at its own length")
{
    // A material shades by distance from a centre, so an arm two radii long and a tenth
    // wide has to be lit as a thin cylinder. Given half its length it would be lit as a
    // ball two radii across, and the whole arm would sit inside one soft highlight.
    const Render::Shape s = Parse(R"([
        { "form": "disc", "radius": 0.5 },
        { "form": "capsule", "at": [1.0, 0.0], "length": 2.0, "width": 0.1 },
        { "form": "ring", "radius": 1.5, "width": 0.08 }
    ])");

    const std::vector<Render::Piece> p = Render::Compose(s, At({ 0, 0 }, 100.0f, 0.0f, 1, 1.0f));
    REQUIRE(p.size() == 3);

    CHECK(Render::ShadeRadius(p[0]) == doctest::Approx(50.0f));   // a disc is its radius
    CHECK(Render::ShadeRadius(p[1]) == doctest::Approx(5.0f));    // an arm is its half-width
    CHECK(Render::ShadeRadius(p[2]) == doctest::Approx(150.0f));  // a ring is its radius

    SUBCASE("and every shipped part has one, or it is shaded as a point")
    {
        REQUIRE(Archetypes::Load(std::string(TEST_DATA_DIR) + "archetypes.json"));
        for (const Archetype& a : Archetypes::All())
        {
            if (a.visual.shape.Empty())
                continue;
            INFO("archetype: ", a.id);
            for (const Render::Piece& piece : Render::Compose(
                     a.visual.shape,
                     At({ 0, 0 }, a.defaultSize > 0.0f ? a.defaultSize : 100.0f, 0.0f, 1, 1.0f)))
                CHECK(Render::ShadeRadius(piece) > 0.0f);
        }
    }
}

TEST_CASE("the shapes that ship are ones the game can read")
{
    // A composition lives in archetypes.json, so a typo in it is a broken build that
    // compiles. The load itself refuses an unknown form, which is what makes this a check
    // rather than a hope.
    REQUIRE(Archetypes::Load(std::string(TEST_DATA_DIR) + "archetypes.json"));

    int composed = 0;
    for (const Archetype& a : Archetypes::All())
    {
        if (a.visual.shape.Empty())
            continue;
        composed++;
        INFO("archetype: ", a.id);
        // The point of the milestone: an object is a composition, not one figure.
        CHECK(a.visual.shape.parts.size() >= 2);

        const std::vector<Render::Piece> pieces = Render::Compose(
            a.visual.shape,
            At({ 0.0f, 0.0f }, a.defaultSize > 0.0f ? a.defaultSize : 100.0f, 0.0f, 1, 1.0f));
        CHECK_FALSE(pieces.empty());
        for (const Render::Piece& p : pieces)
        {
            // A part with no extent is a part nobody can see, and it is always a mistake
            // in the data rather than a choice.
            const bool round = p.form == Render::Form::Disc || p.form == Render::Form::Ring ||
                               p.form == Render::Form::Polygon;
            CHECK((round ? p.radius : p.length) > 0.0f);
        }
    }
    CHECK(composed > 0);
}

TEST_CASE(
    "a row lays copies along a line, an arc spans its angles, a tint is the part's own (#214)")
{
    // Ribs down a hull: five, half a radius apart, starting at the stern.
    const Render::Shape ribs = Parse(R"([
        { "form": "bar", "at": [-1.0, 0.0], "length": 0.1, "width": 0.6,
          "row": { "count": 5, "step": [0.5, 0.0] } } ])");
    const auto          pieces = Render::Compose(ribs, At({ 0.0f, 0.0f }, 100.0f, 0.0f, 1, 1.0f));
    REQUIRE(pieces.size() == 5);
    for (int k = 0; k < 5; k++)
        CHECK(pieces[k].pos.x == doctest::Approx(-100.0f + 50.0f * k));
    CHECK(Render::Extent(ribs) >= 1.0f);  // the row's far end counts, not only its start

    // A row turns with the object, like everything else.
    const auto turned = Render::Compose(ribs, At({ 0.0f, 0.0f }, 100.0f, PI / 2.0f, 1, 1.0f));
    CHECK(turned[4].pos.y == doctest::Approx(100.0f).epsilon(0.01));

    // A broken docking ring: an arc from 20 to 160 degrees, carried round by the heading,
    // and a mirrored one runs the other way.
    const Render::Shape ring = Parse(R"([
        { "form": "arc", "radius": 1.2, "width": 0.1, "from": 20, "to": 160, "mirror": true } ])");
    const auto          arcs = Render::Compose(ring, At({ 0.0f, 0.0f }, 100.0f, 0.0f, 1, 1.0f));
    REQUIRE(arcs.size() == 2);
    CHECK(arcs[0].form == Render::Form::Arc);
    CHECK(arcs[0].arcFrom == doctest::Approx(20.0f));
    CHECK(arcs[0].arcTo == doctest::Approx(160.0f));
    CHECK(arcs[1].arcFrom == doctest::Approx(-160.0f));
    CHECK(arcs[1].arcTo == doctest::Approx(-20.0f));

    // A red lamp on a grey hull.
    const Render::Shape lamp =
        Parse(R"([ { "form": "disc", "radius": 0.05, "role": "light", "tint": [255, 40, 30] } ])");
    const auto lit = Render::Compose(lamp, At({ 0.0f, 0.0f }, 100.0f, 0.0f, 1, 1.0f));
    REQUIRE(lit.size() == 1);
    CHECK(lit[0].tint.r == 255);
    CHECK(lit[0].tint.a == 255);

    // And a misspelt row or a tint that is not a colour is a load error, never a default.
    Render::Shape bad;
    std::string   error;
    CHECK_FALSE(Render::ParseShape(
        nlohmann::json::parse(R"([ { "form": "bar", "row": { "count": 3, "stpe": [1, 0] } } ])"),
        bad, error));
    CHECK_FALSE(error.empty());
    CHECK_FALSE(Render::ParseShape(
        nlohmann::json::parse(R"([ { "form": "disc", "tint": "red" } ])"), bad, error));
}

TEST_CASE("a module is placed by name, in a variant the seed picks (#240)")
{
    std::string error;
    REQUIRE(Render::Modules::Load(std::string(TEST_DATA_DIR) + "modules.json", error));
    CHECK(error.empty());
    REQUIRE(Render::Modules::Find("hatch") != nullptr);
    CHECK(Render::Modules::Find("hatch")->variants.size() >= 2);

    // A round hatch, a tenth of the object, half a radius out: its parts land there.
    const Render::Shape one = Parse(R"([
        { "module": "hatch", "variant": "round", "at": [0.5, 0.0], "scale": 0.1 } ])");
    const auto          pieces = Render::Compose(one, At({ 0.0f, 0.0f }, 100.0f, 0.0f, 1, 10.0f));
    REQUIRE(pieces.size() == Render::Modules::Find("hatch")->variants[1].shape.parts.size());
    for (const auto& p : pieces)
    {
        CHECK(p.pos.x == doctest::Approx(50.0f).epsilon(0.05));
        CHECK(p.radius <= 10.0f);  // a tenth of a hundred-unit object
    }

    // Too small on screen, a module is not drawn at all: it fills in as you approach.
    CHECK(Render::Compose(one, At({ 0.0f, 0.0f }, 100.0f, 0.0f, 1, 0.01f)).empty());

    // Without a pinned variant the seed chooses, and different objects choose differently.
    const Render::Shape any = Parse(R"([ { "module": "turret", "scale": 0.1 } ])");
    std::set<size_t>    counts;
    for (int seed = 1; seed <= 40; seed++)
        counts.insert(Render::Compose(any, At({ 0.0f, 0.0f }, 100.0f, 0.0f, seed, 10.0f)).size());
    CHECK(counts.size() >= 2);

    // A name that means nothing is a load error, said by name.
    Render::Shape bad;
    CHECK_FALSE(
        Render::ParseShape(nlohmann::json::parse(R"([ { "module": "hatchh" } ])"), bad, error));
    CHECK(error.find("hatchh") != std::string::npos);
    CHECK_FALSE(Render::ParseShape(
        nlohmann::json::parse(R"([ { "module": "hatch", "variant": "oval" } ])"), bad, error));
}

TEST_CASE("a range, a palette and a chance make one written part a family (#240)")
{
    const Render::Shape s = Parse(R"([
        { "form": "bar", "length": [0.5, 1.5], "tint": [[200, 0, 0], [0, 200, 0], [0, 0, 200]],
          "row": { "count": [2, 6], "step": [0.2, 0.0] } },
        { "form": "disc", "radius": 0.1, "chance": 0.5 } ])");
    std::set<int>       lengths, rows, colours;
    int                 withDisc = 0;
    for (int seed = 1; seed <= 60; seed++)
    {
        const auto pieces = Render::Compose(s, At({ 0.0f, 0.0f }, 100.0f, 0.0f, seed, 1.0f));
        int        bars = 0;
        for (const auto& p : pieces)
            if (p.form == Render::Form::Bar)
            {
                bars++;
                CHECK(p.length >= 50.0f - 0.01f);
                CHECK(p.length <= 150.0f + 0.01f);
                lengths.insert((int)p.length);
                colours.insert(p.tint.r * 3 + p.tint.g * 2 + p.tint.b);
            }
            else
                withDisc++;
        CHECK(bars >= 2);
        CHECK(bars <= 6);
        rows.insert(bars);
        // The same seed is the same object, every time.
        CHECK(Render::Compose(s, At({ 0.0f, 0.0f }, 100.0f, 0.0f, seed, 1.0f)).size() ==
              pieces.size());
    }
    CHECK(lengths.size() > 10);
    CHECK(rows.size() >= 4);
    CHECK(colours.size() == 3);
    CHECK(withDisc > 10);
    CHECK(withDisc < 50);
}

TEST_CASE("a module with a latitude and a longitude is laid on the planet, not on the disc (#240)")
{
    std::string error;
    REQUIRE(Render::Modules::Load(std::string(TEST_DATA_DIR) + "modules.json", error));

    // A row of three hatches as a "base", facing the viewer, a tenth of the planet across.
    const Render::Shape s = Parse(R"({ "tilt": 0, "parts": [
        { "form": "disc", "radius": 1.0 },
        { "module": "hatch", "variant": "round", "lat": 0, "lon": 0, "scale": 0.1,
          "row": { "count": 3, "step": [0.2, 0.0] }, "spin": 10.0 } ]})");
    Render::Pose        p = At({ 0.0f, 0.0f }, 100.0f, 0.0f, 1, 10.0f);

    // Every piece of it is a surface piece, lit as the body and on the disc; when the
    // planet has turned it to face the viewer, all of it is there.
    int most = 0;
    for (int i = 0; i < 36; i++)
    {
        p.time = (float)i;
        int onSurface = 0;
        for (const Render::Piece& piece : Render::Compose(s, p))
            if (piece.surface)
            {
                onSurface++;
                CHECK(piece.bodyRadius == doctest::Approx(100.0f));
                CHECK(Dist(piece.pos, { 0.0f, 0.0f }) <= 100.0f + 0.01f);
            }
        most = std::max(most, onSurface);
    }
    const size_t perHatch = Render::Modules::Find("hatch")->variants[1].shape.parts.size();
    CHECK(most == (int)(3 * perHatch));

    // The planet's turn carries the whole base round the back and out again.
    int hidden = 0;
    for (int i = 0; i < 36; i++)
    {
        p.time = (float)i;
        if (Render::Compose(s, p).size() == 1)
            hidden++;
    }
    CHECK(hidden > 8);

    // A latitude may be a range, so a crater field is scattered differently per planet.
    const Render::Shape scattered = Parse(R"({ "tilt": 0, "parts": [
        { "module": "hatch", "variant": "round", "lat": [-60, 60], "lon": 0, "scale": 0.1 } ]})");
    std::set<int>       heights;
    for (int seed = 1; seed <= 20; seed++)
        for (const Render::Piece& piece :
             Render::Compose(scattered, At({ 0.0f, 0.0f }, 100.0f, 0.0f, seed, 10.0f)))
            heights.insert((int)piece.pos.y);
    CHECK(heights.size() > 5);
}

TEST_CASE("more modules come in packs, one file each, and an id is still global (#240)")
{
    // Written to the working directory (the build tree under ctest), never into data/.
    MakeDirectory("packs_test_tmp/modules");
    auto write = [](const char* path, const char* body)
    {
        std::ofstream f(path);
        f << body;
    };
    write("packs_test_tmp/modules.json",
          R"({ "modules": [ { "id": "core.thing", "variants": [ { "id": "a", "shape": [
              { "form": "disc", "radius": 1 } ] } ] } ] })");
    write("packs_test_tmp/modules/weapons.json",
          R"({ "modules": [ { "id": "gun", "variants": [ { "id": "a", "shape": [
              { "form": "bar", "length": 2 } ] } ] } ] })");

    std::string error;
    CHECK(Render::Modules::Load("packs_test_tmp/modules.json", error));
    CHECK(error.empty());
    REQUIRE(Render::Modules::Find("gun") != nullptr);
    CHECK(Render::Modules::Find("gun")->pack == "weapons");
    CHECK(Render::Modules::Find("core.thing")->pack == "modules");

    // The same id in two files is an error that names both.
    write("packs_test_tmp/modules/zz.json",
          R"({ "modules": [ { "id": "gun", "variants": [ { "id": "b", "shape": [
              { "form": "bar", "length": 1 } ] } ] } ] })");
    CHECK_FALSE(Render::Modules::Load("packs_test_tmp/modules.json", error));
    CHECK(error.find("weapons") != std::string::npos);
    CHECK(error.find("zz") != std::string::npos);

    std::remove("packs_test_tmp/modules/zz.json");
    std::remove("packs_test_tmp/modules/weapons.json");
    std::remove("packs_test_tmp/modules.json");
    std::string reload;
    REQUIRE(Render::Modules::Load(std::string(TEST_DATA_DIR) + "modules.json", reload));
}

TEST_CASE("an arc's ends, a blink and a spin may be ranges; a wrong kind is an error, not a crash")
{
    const Render::Shape s = Parse(R"([
        { "form": "arc", "radius": 1, "width": 0.1, "from": [0, 40], "to": [90, 180],
          "blink": [1, 3], "spin": [-5, 5] } ])");
    std::set<int>       ends;
    for (int seed = 1; seed <= 20; seed++)
        for (const Render::Piece& p :
             Render::Compose(s, At({ 0.0f, 0.0f }, 100.0f, 0.0f, seed, 1.0f)))
            ends.insert((int)(p.arcTo - p.arcFrom));
    CHECK(ends.size() > 5);

    // Found by an agent writing a pack: a value of the wrong kind used to throw.
    Render::Shape bad;
    std::string   error;
    CHECK_FALSE(Render::ParseShape(
        nlohmann::json::parse(R"([ { "form": "disc", "radius": "large" } ])"), bad, error));
    CHECK_FALSE(error.empty());
    CHECK_FALSE(Render::ParseShape(
        nlohmann::json::parse(R"([ { "form": "disc", "filled": [1, 2] } ])"), bad, error));
}

TEST_CASE("a shape's variables are rolled once and shared by every part that names them (#240)")
{
    const Render::Shape s =
        Parse(R"({ "vars": { "len": [0.5, 1.5], "paint": [[200, 0, 0], [0, 200, 0], [0, 0, 200]] },
        "parts": [
            { "form": "bar", "at": [0, -0.5], "length": "$len", "tint": "$paint" },
            { "form": "bar", "at": [0, 0.5], "length": "$len", "tint": "$paint" } ] })");
    std::set<int> lengths, colours;
    for (int seed = 1; seed <= 40; seed++)
    {
        const auto pieces = Render::Compose(s, At({ 0.0f, 0.0f }, 100.0f, 0.0f, seed, 1.0f));
        REQUIRE(pieces.size() == 2);
        // The same roll on both: a wing's hull and its trim at one sweep, not two.
        CHECK(pieces[0].length == doctest::Approx(pieces[1].length));
        CHECK(pieces[0].tint.r == pieces[1].tint.r);
        CHECK(pieces[0].tint.g == pieces[1].tint.g);
        lengths.insert((int)pieces[0].length);
        colours.insert(pieces[0].tint.r * 3 + pieces[0].tint.g * 2 + pieces[0].tint.b);
    }
    CHECK(lengths.size() > 10);  // ...and a different roll on another object
    CHECK(colours.size() == 3);

    // A name nobody declared, and a colour used as a number, are load errors that say so.
    Render::Shape bad;
    std::string   error;
    CHECK_FALSE(Render::ParseShape(
        nlohmann::json::parse(R"([ { "form": "bar", "length": "$len" } ])"), bad, error));
    CHECK(error.find("len") != std::string::npos);
    CHECK_FALSE(Render::ParseShape(
        nlohmann::json::parse(
            R"({ "vars": { "c": [[1, 2, 3]] }, "parts": [ { "form": "bar", "length": "$c" } ] })"),
        bad, error));
}

TEST_CASE("a centred row stays balanced on its place whatever count it rolls (#240)")
{
    const Render::Shape s = Parse(R"([
        { "form": "disc", "radius": 0.05, "at": [0.3, 0.0],
          "row": { "count": [2, 7], "step": [0.1, 0.0], "centred": true } } ])");
    std::set<size_t>    counts;
    for (int seed = 1; seed <= 30; seed++)
    {
        const auto pieces = Render::Compose(s, At({ 0.0f, 0.0f }, 100.0f, 0.0f, seed, 1.0f));
        float      sum = 0.0f;
        for (const auto& p : pieces)
            sum += p.pos.x;
        CHECK(sum / (float)pieces.size() == doctest::Approx(30.0f).epsilon(0.01));
        counts.insert(pieces.size());
    }
    CHECK(counts.size() >= 4);
}

TEST_CASE("a module's variables are rolled per placed copy, shared within it (#240)")
{
    MakeDirectory("vars_test_tmp");
    {
        std::ofstream f("vars_test_tmp/modules.json");
        f << R"({ "modules": [ { "id": "pod", "variants": [ { "id": "a", "shape": {
            "vars": { "n": [2, 6] },
            "parts": [
                { "form": "disc", "radius": 0.1, "row": { "count": "$n", "step": [0.2, 0] } },
                { "form": "bar", "role": "trim", "length": 0.1, "width": 0.1, "at": [0, 0.3],
                  "row": { "count": "$n", "step": [0.2, 0] } } ] } } ] } ] })";
    }
    std::string error;
    REQUIRE(Render::Modules::Load("vars_test_tmp/modules.json", error));
    const Render::Shape s = Parse(R"([ { "module": "pod", "scale": 0.5, "at": [0, -0.5] },
                                       { "module": "pod", "scale": 0.5, "at": [0, 0.5] } ])");
    bool                differed = false;
    for (int seed = 1; seed <= 30; seed++)
    {
        int tubes[2] = { 0, 0 }, caps[2] = { 0, 0 };
        for (const auto& p : Render::Compose(s, At({ 0.0f, 0.0f }, 100.0f, 0.0f, seed, 10.0f)))
            (p.form == Render::Form::Disc ? tubes : caps)[p.pos.y > 0.0f ? 1 : 0]++;
        // Tubes and their caps agree within a pod...
        CHECK(tubes[0] == caps[0]);
        CHECK(tubes[1] == caps[1]);
        // ...and two pods on one object need not.
        differed = differed || tubes[0] != tubes[1];
    }
    CHECK(differed);
    std::remove("vars_test_tmp/modules.json");
    REQUIRE(Render::Modules::Load(std::string(TEST_DATA_DIR) + "modules.json", error));
}

TEST_CASE("a row may bend and taper, and a part may turn about a joint (#240)")
{
    // Twelve copies turning 30 degrees each close into a ring: every copy is as far from
    // the ring's middle as the others.
    const Render::Shape ring = Parse(R"([ { "form": "disc", "radius": 0.05, "at": [0, 0],
        "row": { "count": 12, "step": [0.2, 0], "turn": 30, "centred": true } } ])");
    const auto          pieces = Render::Compose(ring, At({ 0.0f, 0.0f }, 100.0f, 0.0f, 1, 1.0f));
    REQUIRE(pieces.size() == 12);
    const float r0 = Dist(pieces[0].pos, { 0.0f, 0.0f });
    for (const auto& p : pieces)
        CHECK(Dist(p.pos, { 0.0f, 0.0f }) == doctest::Approx(r0).epsilon(0.02));
    CHECK(r0 > 20.0f);

    // Each copy of a tapering row is smaller than the one before.
    const Render::Shape taper = Parse(R"([ { "form": "disc", "radius": 0.2,
        "row": { "count": 4, "step": [0.3, 0], "taper": 0.5 } } ])");
    const auto          t = Render::Compose(taper, At({ 0.0f, 0.0f }, 100.0f, 0.0f, 1, 1.0f));
    REQUIRE(t.size() == 4);
    CHECK(t[3].radius == doctest::Approx(t[0].radius * 0.125f));

    // A jib of length 1 hinged at its left end: turned 90 degrees, that end stays put and
    // the middle swings to below it.
    const Render::Shape jib = Parse(R"([ { "form": "bar", "length": 1.0, "width": 0.1,
        "at": [0.5, 0], "angle": 90, "pivot": [-0.5, 0] } ])");
    const auto          j = Render::Compose(jib, At({ 0.0f, 0.0f }, 100.0f, 0.0f, 1, 1.0f));
    REQUIRE(j.size() == 1);
    CHECK(j[0].pos.x == doctest::Approx(0.0f).epsilon(0.01));
    CHECK(j[0].pos.y == doctest::Approx(50.0f).epsilon(0.01));
}

TEST_CASE("parts in a group come and go together (#240)")
{
    const Render::Shape s = Parse(R"([
        { "form": "disc", "radius": 0.2, "chance": 0.5, "group": "lamp" },
        { "form": "ring", "radius": 0.3, "width": 0.05, "chance": 0.5, "group": "lamp" } ])");
    int                 both = 0, none = 0;
    for (int seed = 1; seed <= 60; seed++)
    {
        const size_t n = Render::Compose(s, At({ 0.0f, 0.0f }, 100.0f, 0.0f, seed, 1.0f)).size();
        CHECK(n != 1);  // never the lamp without its housing
        (n == 2 ? both : none)++;
    }
    CHECK(both > 10);
    CHECK(none > 10);
}

TEST_CASE("a night-side part says so on its piece, through a module too (#240)")
{
    const Render::Shape s = Parse(R"([
        { "form": "disc", "radius": 0.1, "role": "light", "onlyDark": true },
        { "form": "disc", "radius": 0.1, "at": [0.5, 0] } ])");
    const auto          pieces = Render::Compose(s, At({ 0.0f, 0.0f }, 100.0f, 0.0f, 1, 1.0f));
    REQUIRE(pieces.size() == 2);
    CHECK(pieces[0].onlyDark);
    CHECK_FALSE(pieces[1].onlyDark);

    std::string error;
    REQUIRE(Render::Modules::Load(std::string(TEST_DATA_DIR) + "modules.json", error));
    const Render::Shape city =
        Parse(R"([ { "module": "settlement", "variant": "city", "scale": 0.3 } ])");
    bool any = false;
    for (const auto& p : Render::Compose(city, At({ 0.0f, 0.0f }, 100.0f, 0.0f, 1, 10.0f)))
        any = any || (p.role == Render::Role::Light && p.onlyDark);
    CHECK(any);
}

TEST_CASE("a trapezoid, a jagged polygon and a soft glow are carried to the piece (#240)")
{
    const Render::Shape s = Parse(R"([
        { "form": "chevron", "length": 1, "width": 0.6, "tip": [0.3, 0.5] },
        { "form": "polygon", "sides": 7, "radius": 0.5, "jagged": 0.3 },
        { "form": "disc", "radius": 0.8, "soft": true, "alpha": 0.3 } ])");
    const auto          a = Render::Compose(s, At({ 0.0f, 0.0f }, 100.0f, 0.0f, 1, 1.0f));
    const auto          b = Render::Compose(s, At({ 0.0f, 0.0f }, 100.0f, 0.0f, 2, 1.0f));
    REQUIRE(a.size() == 3);
    CHECK(a[0].tip >= 0.3f);
    CHECK(a[0].tip <= 0.5f);
    CHECK(a[1].jagged == doctest::Approx(0.3f));
    // Two rocks are pulled in at different corners; one rock is the same every frame.
    CHECK(a[1].jagSeed != b[1].jagSeed);
    CHECK(a[1].jagSeed == Render::Compose(s, At({ 0.0f, 0.0f }, 100.0f, 0.0f, 1, 1.0f))[1].jagSeed);
    CHECK(a[2].soft);
    CHECK_FALSE(a[0].soft);
}

TEST_CASE("a soft part inside a module is still soft where the module is placed (#240)")
{
    MakeDirectory("soft_test_tmp");
    {
        std::ofstream f("soft_test_tmp/modules.json");
        f << R"({ "modules": [ { "id": "glow", "variants": [ { "id": "a", "shape": [
            { "form": "disc", "radius": 0.6, "soft": true, "alpha": 0.2 },
            { "form": "disc", "radius": 0.2, "at": [0.5, 0] } ] } ] } ] })";
    }
    std::string error;
    REQUIRE(Render::Modules::Load("soft_test_tmp/modules.json", error));
    const Render::Shape s = Parse(R"([
        { "module": "glow", "at": [1.2, 0], "scale": 0.5, "repeat": 3 },
        { "module": "glow", "at": [0, 1], "scale": 0.5, "mirror": true } ])");
    const auto          pieces = Render::Compose(s, At({ 0.0f, 0.0f }, 100.0f, 0.0f, 1, 10.0f));
    int                 soft = 0, solid = 0;
    for (const auto& p : pieces)
        (p.soft ? soft : solid)++;
    CHECK(soft == 3 + 2);
    CHECK(solid == soft);

    // On a sphere too, where it is in view: the seed turns the planet, so look at several.
    const Render::Shape planet =
        Parse(R"([ { "module": "glow", "lat": 0, "lon": 0, "scale": 0.3 } ])");
    bool softOnSphere = false;
    for (int seed = 1; seed <= 12; seed++)
        for (const auto& p : Render::Compose(planet, At({ 0.0f, 0.0f }, 100.0f, 0.0f, seed, 10.0f)))
            softOnSphere = softOnSphere || (p.surface && p.soft);
    CHECK(softOnSphere);
    std::remove("soft_test_tmp/modules.json");
    REQUIRE(Render::Modules::Load(std::string(TEST_DATA_DIR) + "modules.json", error));
}

TEST_CASE("a variable may be negated, scaled and offset; a row may go round a centre (#240)")
{
    // A pair of jaws opening together: one at $a, the other at -$a.
    const Render::Shape jaws = Parse(R"({ "vars": { "a": [10, 40], "len": [0.4, 0.8] }, "parts": [
        { "form": "bar", "angle": "$a", "length": "$len" },
        { "form": "bar", "angle": "-$a", "length": "$len*0.5+0.1" } ] })");
    for (int seed = 1; seed <= 20; seed++)
    {
        const auto p = Render::Compose(jaws, At({ 0.0f, 0.0f }, 100.0f, 0.0f, seed, 1.0f));
        REQUIRE(p.size() == 2);
        CHECK(p[0].angle == doctest::Approx(-p[1].angle));
        CHECK(p[1].length == doctest::Approx(p[0].length * 0.5f + 10.0f));
    }
    Render::Shape bad;
    std::string   error;
    CHECK_FALSE(Render::ParseShape(
        nlohmann::json::parse(
            R"({ "vars": { "a": [0, 1] }, "parts": [ { "form": "bar", "angle": "$a*x" } ] })"),
        bad, error));
    CHECK_FALSE(error.empty());

    // A ring whose count is a range still closes: every copy at the radius, evenly spaced.
    const Render::Shape ring = Parse(R"([ { "form": "disc", "radius": 0.05, "at": [0, 0],
        "row": { "count": [5, 9], "ring": 0.6 } } ])");
    std::set<size_t>    counts;
    for (int seed = 1; seed <= 30; seed++)
    {
        const auto p = Render::Compose(ring, At({ 0.0f, 0.0f }, 100.0f, 0.0f, seed, 1.0f));
        counts.insert(p.size());
        Vector2 sum = { 0.0f, 0.0f };
        for (const auto& q : p)
        {
            CHECK(Dist(q.pos, { 0.0f, 0.0f }) == doctest::Approx(60.0f).epsilon(0.01));
            sum = { sum.x + q.pos.x, sum.y + q.pos.y };
        }
        CHECK(std::fabs(sum.x) < 0.5f);  // closed: balanced about the centre
        CHECK(std::fabs(sum.y) < 0.5f);
    }
    CHECK(counts.size() >= 3);

    // A fan: spread over 90 degrees about the part's own direction, symmetric.
    const Render::Shape fan = Parse(R"([ { "form": "bar", "length": 0.2, "at": [0, 0],
        "row": { "count": 3, "ring": 0.5, "spread": 90 } } ])");
    const auto          f = Render::Compose(fan, At({ 0.0f, 0.0f }, 100.0f, 0.0f, 1, 1.0f));
    REQUIRE(f.size() == 3);
    CHECK(f[1].pos.x == doctest::Approx(50.0f));
    CHECK(f[0].pos.y == doctest::Approx(-f[2].pos.y));
}

TEST_CASE("a variable's offset may be a range each part rolls on its own (#240)")
{
    // Two rims on one crater: both follow its radius, each a step outside it of its own.
    const Render::Shape rims = Parse(R"({ "vars": { "r": [0.2, 0.4] }, "parts": [
        { "form": "disc", "radius": "$r" },
        { "form": "ring", "radius": "$r+[0.02,0.08]" },
        { "form": "ring", "radius": "$r-[0.05,0.01]" } ] })");
    std::set<int>       steps;
    for (int seed = 1; seed <= 30; seed++)
    {
        const auto p = Render::Compose(rims, At({ 0.0f, 0.0f }, 100.0f, 0.0f, seed, 1.0f));
        REQUIRE(p.size() == 3);
        const float out = p[1].radius - p[0].radius, in = p[0].radius - p[2].radius;
        CHECK(out >= 2.0f - 1e-3f);
        CHECK(out <= 8.0f + 1e-3f);
        CHECK(in >= 1.0f - 1e-3f);
        CHECK(in <= 5.0f + 1e-3f);
        steps.insert((int)(out * 10.0f));
        CHECK(p[1].radius ==
              Render::Compose(rims, At({ 0.0f, 0.0f }, 100.0f, 0.0f, seed, 1.0f))[1].radius);
    }
    CHECK(steps.size() >= 5);

    for (const char* wrong : { "$r+[0.1]", "$r+[0.1,]", "$r+[a,b]", "$r+[0.1,0.2" })
    {
        Render::Shape     bad;
        std::string       error;
        const std::string text =
            std::string(R"({ "vars": { "r": [0, 1] }, "parts": [ { "form": "disc", "radius": ")") +
            wrong + "\" } ] }";
        CHECK_FALSE(Render::ParseShape(nlohmann::json::parse(text), bad, error));
        CHECK_FALSE(error.empty());
    }
}

TEST_CASE("a variant with a field nobody reads is refused (#240)")
{
    MakeDirectory("variant_test_tmp");
    {
        std::ofstream f("variant_test_tmp/modules.json");
        f << R"({ "modules": [ { "id": "x", "variants": [ { "id": "a", "shpae": [],
              "shape": [ { "form": "disc" } ] } ] } ] })";
    }
    std::string error;
    CHECK_FALSE(Render::Modules::Load("variant_test_tmp/modules.json", error));
    CHECK(error.find("shpae") != std::string::npos);
    std::remove("variant_test_tmp/modules.json");
    REQUIRE(Render::Modules::Load(std::string(TEST_DATA_DIR) + "modules.json", error));
}

TEST_CASE("a variant is measured by what it draws, not by a circle round each part (#240)")
{
    MakeDirectory("bounds_test_tmp");
    {
        std::ofstream f("bounds_test_tmp/modules.json");
        f << R"({ "modules": [
            { "id": "arc", "variants": [ { "id": "a", "shape": [
                { "form": "arc", "radius": 1, "width": 0.2, "from": 0, "to": 90 } ] } ] },
            { "id": "bar", "variants": [ { "id": "a", "shape": [
                { "form": "bar", "length": 2, "width": 0.4, "angle": 90 } ] } ] },
            { "id": "pill", "variants": [ { "id": "a", "shape": [
                { "form": "capsule", "length": 1, "width": 0.4, "at": [0.5, 0] } ] } ] } ] })";
    }
    std::string error;
    REQUIRE(Render::Modules::Load("bounds_test_tmp/modules.json", error));
    auto box = [](const char* id) { return Render::Modules::Find(id)->variants[0].bounds; };

    // A quarter arc covers one quarter of the plane, not its whole circle.
    const Rectangle arc = box("arc");
    CHECK(arc.x == doctest::Approx(0.0f).epsilon(0.01));
    CHECK(arc.width == doctest::Approx(1.0f).epsilon(0.01));
    CHECK(arc.height == doctest::Approx(1.0f).epsilon(0.01));

    // A turned bar is its turned rectangle: long one way, thin the other.
    const Rectangle bar = box("bar");
    CHECK(bar.width == doctest::Approx(0.4f).epsilon(0.01));
    CHECK(bar.height == doctest::Approx(2.0f).epsilon(0.01));

    // A capsule reaches its caps and no further, wherever it is placed.
    const Rectangle pill = box("pill");
    CHECK(pill.width == doctest::Approx(1.4f).epsilon(0.01));
    CHECK(pill.height == doctest::Approx(0.4f).epsilon(0.01));

    std::remove("bounds_test_tmp/modules.json");
    REQUIRE(Render::Modules::Load(std::string(TEST_DATA_DIR) + "modules.json", error));
}

TEST_CASE("a capsule's end socket is at the tip of its cap, where the drawing ends (#240)")
{
    Render::Part pill;
    pill.form = Render::Form::Capsule;
    pill.length = 1.0f;
    pill.width = 0.4f;
    pill.section = true;
    int ends = 0;
    for (const auto& k : Render::Sockets({ pill }))
        if (k.type == "end")
        {
            CHECK(std::fabs(k.pos.x) == doctest::Approx(0.7f));
            ends++;
        }
    CHECK(ends == 2);
}

TEST_CASE(
    "a section exposes sockets: edges, ends and a centreline on a block, a rim on a disc (#240)")
{
    Render::Part bar;
    bar.form = Render::Form::Bar;
    bar.length = 1.0f;
    bar.width = 0.2f;
    bar.section = true;
    const auto s = Render::Sockets({ bar });
    int        edges = 0, ends = 0, tops = 0;
    for (const auto& k : s)
    {
        edges += k.type == "edge";
        ends += k.type == "end";
        tops += k.type == "top";
        if (k.type == "edge")
            CHECK(std::fabs(std::fabs(k.pos.y) - 0.1f) < 1e-4f);  // on the long sides
    }
    CHECK(ends == 2);
    CHECK(edges == 2 * tops);
    CHECK(tops >= 3);

    Render::Part disc;
    disc.form = Render::Form::Disc;
    disc.radius = 0.6f;
    disc.section = true;
    for (const auto& k : Render::Sockets({ disc }))
        if (k.type == "edge")
            CHECK(Dist(k.pos, { 0.0f, 0.0f }) == doctest::Approx(0.6f).epsilon(0.01));
}

TEST_CASE("a kit places modules by seed: in mirrored pairs, evenly, leaving room plain (#240)")
{
    std::string error;
    REQUIRE(Render::Modules::Load(std::string(TEST_DATA_DIR) + "modules.json", error));
    const Render::Shape       ship = Parse(R"({
        "sections": [ { "form": "bar", "length": 1.6, "width": 0.4, "pitch": 0.2 } ],
        "kit": { "symmetry": "bilateral", "plain": 0.4,
                 "modules": [ { "of": "hatch", "count": [2, 6], "on": "edge" } ] },
        "parts": [] })");
    std::vector<Render::Part> sections;
    for (const auto& p : ship.parts)
        if (p.section)
            sections.push_back(p);
    REQUIRE(sections.size() == 1);
    const size_t    sockets = Render::Sockets(sections).size();
    std::set<int>   counts;
    std::set<float> firstX;
    for (int seed = 1; seed <= 30; seed++)
    {
        const auto placed = Render::PlaceKit(ship.kit, sections, seed);
        counts.insert((int)placed.size());
        // Never more than the plain rule allows on the section.
        CHECK(placed.size() <= (size_t)((float)sockets * 0.6f) + 1);
        // Bilateral: every module on one side has its reflection on the other.
        int mirrored = 0;
        for (const auto& p : placed)
            mirrored += p.mirrorOnly;
        CHECK(mirrored * 2 == (int)placed.size());
        // One variant along the whole line: a row of the same hatch.
        for (const auto& p : placed)
            CHECK(p.variant == placed[0].variant);
        if (!placed.empty())
            firstX.insert(placed[0].at.x);
        // The same seed is the same object.
        CHECK(Render::PlaceKit(ship.kit, sections, seed).size() == placed.size());
    }
    CHECK(counts.size() >= 2);
    CHECK(firstX.size() >= 2);

    // Composed, the kit's modules are drawn like written ones: more than the section alone.
    CHECK(Render::Compose(ship, At({ 0.0f, 0.0f }, 100.0f, 0.0f, 3, 10.0f)).size() > 1);

    Render::Shape bad;
    CHECK_FALSE(Render::ParseShape(nlohmann::json::parse(R"({ "sections": [], "parts": [],
            "kit": { "modules": [ { "of": "hatchh" } ] } })"),
                                   bad, error));
    CHECK(error.find("hatchh") != std::string::npos);
    CHECK_FALSE(Render::ParseShape(
        nlohmann::json::parse(R"({ "parts": [], "kit": { "modules": [ { "of": "#nothing" } ] } })"),
        bad, error));
}

TEST_CASE("a radial kit puts the same module in the same place on every arm (#240)")
{
    std::string error;
    REQUIRE(Render::Modules::Load(std::string(TEST_DATA_DIR) + "modules.json", error));
    const Render::Shape       hub = Parse(R"({
        "sections": [ { "form": "bar", "at": [0.8, 0], "length": 0.8, "width": 0.2, "repeat": 3 } ],
        "kit": { "symmetry": "radial", "modules": [ { "of": "hatch", "count": 2, "on": "edge" } ] },
        "parts": [] })");
    std::vector<Render::Part> sections;
    for (const auto& p : hub.parts)
        if (p.section)
            sections.push_back(p);
    for (int seed = 1; seed <= 10; seed++)
    {
        const auto placed = Render::PlaceKit(hub.kit, sections, seed);
        REQUIRE(placed.size() == 6);  // two per arm, three arms
        // Each placement has partners at the same distance from the centre.
        for (const auto& p : placed)
        {
            int same = 0;
            for (const auto& q : placed)
                same += std::fabs(Dist(p.at, { 0, 0 }) - Dist(q.at, { 0, 0 })) < 1e-3f;
            CHECK(same >= 3);
        }
    }
}

TEST_CASE("a kit mounts a module by its box and draws it in its layer (#240)")
{
    std::string error;
    REQUIRE(Render::Modules::Load(std::string(TEST_DATA_DIR) + "modules.json", error));
    // "on": the hatches lie wholly on the hull, inside its edge, however each hatch is drawn
    // about its own origin.
    const Render::Shape hull = Parse(R"({
        "sections": [ { "form": "bar", "length": 2.0, "width": 0.6, "pitch": 0.2 } ],
        "kit": { "symmetry": "bilateral", "modules": [
            { "of": "hatch", "on": "edge", "count": 2, "scale": 0.08, "z": 2 } ] },
        "parts": [] })");
    for (int seed = 1; seed <= 8; seed++)
        for (const Render::Piece& p :
             Render::Compose(hull, At({ 0.0f, 0.0f }, 100.0f, 0.0f, seed, 10.0f)))
            if (p.z == 2)
                CHECK(std::fabs(p.pos.y) <= 30.0f + 0.5f);  // inside the 60-wide hull

    // A part below the hull is drawn before it, whatever order it was written in.
    const Render::Shape layered = Parse(R"([
        { "form": "disc", "radius": 1.0 },
        { "form": "bar", "role": "panel", "at": [-1, 0], "z": -1 } ])");
    const auto pieces = Render::Compose(layered, At({ 0.0f, 0.0f }, 100.0f, 0.0f, 1, 1.0f));
    REQUIRE(pieces.size() == 2);
    CHECK(pieces[0].form == Render::Form::Bar);
}

TEST_CASE("a module part or a kit line may allow some variants or bar some (#240)")
{
    std::string error;
    REQUIRE(Render::Modules::Load(std::string(TEST_DATA_DIR) + "modules.json", error));
    const Render::Module* engine = Render::Modules::Find("engine");
    REQUIRE(engine != nullptr);

    // With no lists every variant is allowed, in the module's order: the seed's choice by
    // index is the choice it made before the lists existed.
    CHECK(Render::AllowedVariants(*engine, {}, {}).size() == engine->variants.size());
    const auto only = Render::AllowedVariants(*engine, { "bell", "cluster" }, {});
    REQUIRE(only.size() == 2);
    CHECK(only[0]->id == "bell");
    const auto but = Render::AllowedVariants(*engine, {}, { "ion" });
    CHECK(but.size() == engine->variants.size() - 1);

    // Written on a part, every seed draws one of the allowed variants exactly as if it had
    // been pinned, and over many seeds each of them turns up.
    auto same = [](const std::vector<Render::Piece>& a, const std::vector<Render::Piece>& b)
    {
        if (a.size() != b.size())
            return false;
        for (size_t k = 0; k < a.size(); k++)
            if (a[k].form != b[k].form || Dist(a[k].pos, b[k].pos) > 1e-3f)
                return false;
        return true;
    };
    const Render::Shape listed =
        Parse(R"([ { "module": "turret", "scale": 0.1, "variants": ["heavy", "missile"] } ])");
    const Render::Shape heavy =
        Parse(R"([ { "module": "turret", "scale": 0.1, "variant": "heavy" } ])");
    const Render::Shape missile =
        Parse(R"([ { "module": "turret", "scale": 0.1, "variant": "missile" } ])");
    int sawHeavy = 0, sawMissile = 0;
    for (int seed = 1; seed <= 40; seed++)
    {
        const auto pose = At({ 0.0f, 0.0f }, 100.0f, 0.0f, seed, 10.0f);
        const auto got = Render::Compose(listed, pose);
        const bool h = same(got, Render::Compose(heavy, pose));
        const bool m = same(got, Render::Compose(missile, pose));
        CHECK((h || m));
        sawHeavy += h;
        sawMissile += m;
    }
    CHECK(sawHeavy > 0);
    CHECK(sawMissile > 0);

    // On a kit line: the placed parts carry only allowed variants, and the same seed is the
    // same choice.
    const Render::Shape       ship = Parse(R"({
        "sections": [ { "form": "bar", "length": 1.6, "width": 0.4, "pitch": 0.2 } ],
        "kit": { "modules": [ { "of": "engine", "count": 1, "on": "end", "except": ["ion"] },
                              { "of": "#opening", "count": 1, "on": "edge",
                                "variants": ["round"] } ] },
        "parts": [] })");
    std::vector<Render::Part> sections;
    for (const auto& p : ship.parts)
        if (p.section)
            sections.push_back(p);
    std::set<std::string> engines;
    for (int seed = 1; seed <= 40; seed++)
    {
        const auto placed = Render::PlaceKit(ship.kit, sections, seed);
        for (const auto& p : placed)
        {
            CHECK(p.variant != "ion");
            if (p.module == "engine")
                engines.insert(p.variant);
            else
                CHECK(p.variant == "round");  // hatch and vent both have one
        }
    }
    CHECK(engines.size() == 2);

    // Mistakes are load errors that say so.
    Render::Shape bad;
    CHECK_FALSE(Render::ParseShape(
        nlohmann::json::parse(R"([ { "module": "engine", "variants": ["warp"] } ])"), bad, error));
    CHECK(error.find("warp") != std::string::npos);
    CHECK_FALSE(Render::ParseShape(
        nlohmann::json::parse(
            R"([ { "module": "engine", "except": ["bell", "cluster", "ion"] } ])"),
        bad, error));
    CHECK_FALSE(Render::ParseShape(
        nlohmann::json::parse(
            R"([ { "module": "engine", "variant": "bell", "except": ["ion"] } ])"),
        bad, error));
    CHECK_FALSE(Render::ParseShape(
        nlohmann::json::parse(R"([ { "module": "engine", "variants": "bell" } ])"), bad, error));
    // A tag's lines check against every module carrying it: a hatch's id is fine on
    // #opening, a turret's is not.
    CHECK(Render::ParseShape(nlohmann::json::parse(R"({ "parts": [], "kit": { "modules": [
        { "of": "#opening", "variants": ["blast-door"] } ] } })"),
                             bad, error));
    CHECK_FALSE(Render::ParseShape(nlohmann::json::parse(R"({ "parts": [], "kit": { "modules": [
        { "of": "#opening", "variants": ["heavy"] } ] } })"),
                                   bad, error));
    CHECK(error.find("heavy") != std::string::npos);
}

TEST_CASE("a module whose place is taken goes to the nearest free one, not nowhere (#240)")
{
    std::string error;
    REQUIRE(Render::Modules::Load(std::string(TEST_DATA_DIR) + "modules.json", error));
    // Two lines that both want the middle of the same top line: the second must still land.
    const Render::Shape       s = Parse(R"({
        "sections": [ { "form": "bar", "length": 2.0, "width": 0.3, "pitch": 0.2 } ],
        "kit": { "plain": 0.2, "modules": [
            { "of": "hatch", "on": "top", "count": 1, "scale": 0.06 },
            { "of": "vent", "on": "top", "count": 1, "scale": 0.06 } ] },
        "parts": [] })");
    std::vector<Render::Part> sections;
    for (const auto& p : s.parts)
        if (p.section)
            sections.push_back(p);
    const auto placed = Render::PlaceKit(s.kit, sections, 1);
    REQUIRE(placed.size() == 2);
    CHECK(Dist(placed[0].at, placed[1].at) > 0.05f);  // side by side, not on top
}

TEST_CASE("socket rules: a ring's on its band, a chevron's sides, a middle, nothing underneath "
          "(#240)")
{
    // A ring's sockets on the middle of its band, which is drawn from radius - width out.
    Render::Part ring;
    ring.form = Render::Form::Ring;
    ring.radius = 1.0f;
    ring.width = 0.2f;
    ring.section = true;
    int rings = 0;
    for (const auto& k : Render::Sockets({ ring }))
        if (k.type == "ring")
        {
            rings++;
            CHECK(Dist(k.pos, { 0.0f, 0.0f }) == doctest::Approx(0.9f).epsilon(0.01));
        }
    CHECK(rings >= 3);

    // A chevron's slanted sides are edges, on the side and facing square out of it.
    Render::Part wedge;
    wedge.form = Render::Form::Chevron;
    wedge.length = 1.0f;
    wedge.width = 0.6f;
    wedge.pitch = 0.15f;
    wedge.section = true;
    int sides[2] = { 0, 0 };
    for (const auto& k : Render::Sockets({ wedge }))
        if (k.type == "edge")
        {
            sides[k.pos.y > 0.0f ? 1 : 0]++;
            // On the line from the tail corner (-0.5, +-0.3) to the point (0.5, 0).
            CHECK(std::fabs(std::fabs(k.pos.y) - 0.3f * (0.5f - k.pos.x)) < 1e-3f);
            // Square to that side, and out of it.
            const Vector2 n = { std::cos(k.angle * DEG2RAD), std::sin(k.angle * DEG2RAD) };
            CHECK(std::fabs(n.x + n.y * (k.pos.y > 0.0f ? -0.3f : 0.3f)) < 1e-3f);
            CHECK(n.y * k.pos.y > 0.0f);
            CHECK(n.x > 0.0f);
        }
    CHECK(sides[0] >= 2);
    CHECK(sides[0] == sides[1]);

    // One middle per section, at its centre, facing its own way.
    Render::Part bar;
    bar.form = Render::Form::Bar;
    bar.at = { 0.3f, 0.2f };
    bar.angle = 30.0f;
    bar.length = 1.0f;
    bar.width = 0.3f;
    bar.section = true;
    int middles = 0;
    for (const auto& k : Render::Sockets({ bar }))
        if (k.type == "middle")
        {
            middles++;
            CHECK(Dist(k.pos, bar.at) < 1e-4f);
            CHECK(k.angle == doctest::Approx(30.0f));
        }
    CHECK(middles == 1);

    // An arm drawn over a hub: the hub's rim under the arm and the arm's end inside the hub
    // are no places. A panel lying on the hub keeps all of its own.
    Render::Part hub;
    hub.form = Render::Form::Disc;
    hub.radius = 0.5f;
    hub.pitch = 0.1f;
    hub.section = true;
    Render::Part arm;
    arm.form = Render::Form::Bar;
    arm.at = { 0.8f, 0.0f };
    arm.length = 0.8f;
    arm.width = 0.2f;
    arm.section = true;
    Render::Part panel;
    panel.form = Render::Form::Bar;
    panel.at = { -0.1f, 0.0f };
    panel.angle = 90.0f;
    panel.length = 0.5f;
    panel.width = 0.1f;
    panel.pitch = 0.1f;
    panel.section = true;
    int hubEdges = 0, armEnds = 0, panelSockets = 0;
    for (const auto& k : Render::Sockets({ hub, arm, panel }))
    {
        if (k.section == 0 && k.type == "edge")
        {
            hubEdges++;
            CHECK_FALSE((k.pos.x > 0.3f && std::fabs(k.pos.y) < 0.09f));
        }
        if (k.section == 1 && k.type == "end")
        {
            armEnds++;
            CHECK(k.pos.x > 1.0f);
        }
        panelSockets += k.section == 2;
    }
    CHECK(hubEdges > 10);
    CHECK(armEnds == 1);
    CHECK(panelSockets == (int)Render::Sockets({ panel }).size());

    // "on": "middle" is a socket a kit line may name; there is one per section.
    std::string error;
    REQUIRE(Render::Modules::Load(std::string(TEST_DATA_DIR) + "modules.json", error));
    const Render::Shape       tower = Parse(R"({
        "sections": [ { "form": "disc", "at": [0.2, 0.1], "radius": 0.6 } ],
        "kit": { "plain": 0.0, "modules": [ { "of": "hatch", "on": "middle", "count": 3 } ] },
        "parts": [] })");
    std::vector<Render::Part> sections;
    for (const auto& p : tower.parts)
        if (p.section)
            sections.push_back(p);
    CHECK(Render::PlaceKit(tower.kit, sections, 1).size() == 1);
}

TEST_CASE("an object's expansion is kept, and a turning module still turns with the clock (#296)")
{
    std::string error;
    REQUIRE(Render::Modules::Load(std::string(TEST_DATA_DIR) + "modules.json", error));

    // A hatch carried round the centre at 30 degrees a second: kept from one frame to the
    // next, and still a function of the time it is drawn at (#136).
    const Render::Shape s = Parse(R"([
        { "module": "hatch", "variant": "round", "at": [0.5, 0.0], "scale": 0.1, "spin": 30 } ])");
    Render::Pose        pose = At({ 0.0f, 0.0f }, 100.0f, 0.0f, 1, 10.0f);
    const auto          first = Render::Compose(s, pose);
    pose.time = 3.0;  // a quarter of a turn
    const auto later = Render::Compose(s, pose);
    const auto again = Render::Compose(s, pose);
    REQUIRE(!first.empty());
    REQUIRE(first.size() == later.size());
    REQUIRE(later.size() == again.size());
    for (size_t i = 0; i < first.size(); i++)
    {
        // (x, y) turned a quarter, on screen counter-clockwise as every angle in the grammar.
        CHECK(later[i].pos.x == doctest::Approx(-first[i].pos.y).epsilon(0.01));
        CHECK(later[i].pos.y == doctest::Approx(first[i].pos.x).epsilon(0.01));
        CHECK(again[i].pos.x == later[i].pos.x);
        CHECK(again[i].pos.y == later[i].pos.y);
    }
}

TEST_CASE("a shape rebuilt where another one was is expanded again, not handed the old one")
{
    // The cache is keyed by where a shape is, and a shape built in the same place with other
    // parts -- a test, the editor's module page -- must not be given the previous expansion.
    auto radiusOf = [](const char* text)
    {
        const Render::Shape s = Parse(text);
        const auto          p = Render::Compose(s, At({ 0.0f, 0.0f }, 100.0f, 0.0f, 7, 1.0f));
        REQUIRE(p.size() == 1);
        return p[0].radius;
    };
    CHECK(radiusOf(R"([ { "form": "disc", "radius": [0.2, 0.2] } ])") == doctest::Approx(20.0f));
    CHECK(radiusOf(R"([ { "form": "disc", "radius": [0.5, 0.5] } ])") == doctest::Approx(50.0f));
}

TEST_CASE("an object is culled by a reach that covers every piece it can have, at any time")
{
    // Culling by a reach that falls short is a piece of a station vanishing at the edge of
    // the screen. So for every shipped shape, several objects of it and several moments, every
    // piece has to lie inside the reach it is culled by.
    REQUIRE(Archetypes::Load(std::string(TEST_DATA_DIR) + "archetypes.json"));
    for (const Archetype& a : Archetypes::All())
    {
        if (a.visual.shape.Empty())
            continue;
        INFO("archetype: ", a.id);
        for (int seed = 1; seed <= 6; seed++)
        {
            const float reach = 100.0f * Render::Reach(a.visual.shape, seed);
            for (double t : { 0.0, 7.3, 41.0, 333.3 })
            {
                Render::Pose pose = At({ 0.0f, 0.0f }, 100.0f, 0.7f, seed, 1000.0f);
                pose.time = t;
                pose.thrusting = true;
                for (const Render::Piece& p : Render::Compose(a.visual.shape, pose))
                    CHECK(Dist(p.pos, { 0.0f, 0.0f }) + Render::PieceReach(p) <=
                          reach * 1.001f + 0.01f);
            }
        }
    }
}
