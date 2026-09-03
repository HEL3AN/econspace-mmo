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
        Render::Compose(s, { 100.0f, 50.0f }, 90.0f, 0.0f, 1, 1.0f);
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

    const std::vector<Render::Piece> p = Render::Compose(s, { 0.0f, 0.0f }, 100.0f, 0.0f, 1, 1.0f);
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
        Render::Compose(s, { 0.0f, 0.0f }, 10.0f, 0.0f, 1, 1.0f);
    const std::vector<Render::Piece> turned =
        Render::Compose(s, { 0.0f, 0.0f }, 10.0f, (float)M_PI / 2.0f, 1, 1.0f);

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
    CHECK(Render::Compose(s, { 0.0f, 0.0f }, 90.0f, 0.0f, 1, 0.03f).size() == 1);

    // The same station up close: the lamps arrive.
    CHECK(Render::Compose(s, { 0.0f, 0.0f }, 90.0f, 0.0f, 1, 1.0f).size() == 7);
}

TEST_CASE("two of the same thing differ, and neither of them shimmers")
{
    const Render::Shape s = Parse(R"([
        { "form": "capsule", "at": [1.0, 0.0], "repeat": 3, "length": 0.5,
          "jitterAngle": 6.0, "jitterScale": 0.1 }
    ])");

    const std::vector<Render::Piece> a = Render::Compose(s, { 0, 0 }, 100.0f, 0.0f, 7, 1.0f);
    const std::vector<Render::Piece> b = Render::Compose(s, { 0, 0 }, 100.0f, 0.0f, 8, 1.0f);
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
            Render::Compose(s, { 0, 0 }, 100.0f, 0.0f, 7, 1.0f);
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

    const std::vector<Render::Piece> p = Render::Compose(s, { 0, 0 }, 100.0f, 0.0f, 1, 1.0f);
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
            for (const Render::Piece& piece :
                 Render::Compose(a.visual.shape, { 0, 0 },
                                 a.defaultSize > 0.0f ? a.defaultSize : 100.0f, 0.0f, 1, 1.0f))
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

        const std::vector<Render::Piece> pieces =
            Render::Compose(a.visual.shape, { 0.0f, 0.0f },
                            a.defaultSize > 0.0f ? a.defaultSize : 100.0f, 0.0f, 1, 1.0f);
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
