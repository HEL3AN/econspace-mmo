#include <doctest/doctest.h>

#include "core/Archetype.h"
#include "render/Silhouette.h"
#include <cmath>
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
