#include <doctest/doctest.h>

#include "core/Archetype.h"
#include "entities/Star.h"
#include "entities/Station.h"
#include "render/Lighting.h"
#include "render/Scene.h"
#include <cmath>
#include <string>

// Lighting is judged by eye and there is no GPU in CI, so what a lit object *looks* like
// is out of reach here. What is not out of reach is the arithmetic underneath: which
// direction the light comes from, how it falls off, and what happens when a system has
// more than one star -- which is the case this was built as a list for.

namespace
{
// A light source, named so it does not collide with the Star entity below.
Render::Light Source(Vector2 at, Color c = WHITE, float radius = 10000.0f, float intensity = 1.0f)
{
    return { at, c, intensity, radius };
}
}  // namespace

TEST_CASE("light arrives from where the light is, and fades to nothing at its radius")
{
    Render::Lighting lg;
    lg.lights.push_back(Source({ 0.0f, 0.0f }));

    SUBCASE("an object to the right of a star is lit from its left")
    {
        const Render::Lighting::Sample s = lg.At({ 1000.0f, 0.0f });
        CHECK(s.dir.x == doctest::Approx(-1.0f));
        CHECK(s.dir.y == doctest::Approx(0.0f));
        CHECK(s.strength > 0.5f);
    }

    SUBCASE("further away is dimmer")
    {
        CHECK(lg.At({ 8000.0f, 0.0f }).strength < lg.At({ 2000.0f, 0.0f }).strength);
    }

    SUBCASE("past the radius there is no light at all, not a faint one")
    {
        // Deliberate: an inverse-square falloff never reaches zero, so a star across the
        // system would still decide which way an object is lit from.
        const Render::Lighting::Sample s = lg.At({ 10001.0f, 0.0f });
        CHECK(s.strength == doctest::Approx(0.0f));
        CHECK(s.dir.x == doctest::Approx(0.0f));
        CHECK(s.dir.y == doctest::Approx(0.0f));
    }

    SUBCASE("standing inside the light there is no direction to be lit from")
    {
        const Render::Lighting::Sample s = lg.At({ 0.0f, 0.0f });
        CHECK(s.strength == doctest::Approx(1.0f));
        CHECK(s.dir.x == doctest::Approx(0.0f));  // lit from everywhere, which a star is
        CHECK(s.dir.y == doctest::Approx(0.0f));
    }
}

TEST_CASE("a system may have more than one star")
{
    Render::Lighting lg;
    lg.lights.push_back(Source({ -1000.0f, 0.0f }));
    lg.lights.push_back(Source({ 1000.0f, 0.0f }));

    SUBCASE("between two equal stars there is no dark side")
    {
        const Render::Lighting::Sample s = lg.At({ 0.0f, 0.0f });
        CHECK(s.strength == doctest::Approx(1.0f));
        CHECK(std::fabs(s.dir.x) < 0.001f);  // the two cancel
        CHECK(std::fabs(s.dir.y) < 0.001f);
    }

    SUBCASE("the nearer star wins the direction")
    {
        const Render::Lighting::Sample s = lg.At({ 900.0f, 0.0f });
        CHECK(s.dir.x > 0.0f);  // toward the star at +1000, not the one at -1000
    }

    SUBCASE("a red star and a blue one make neither red nor blue light")
    {
        Render::Lighting two;
        two.lights.push_back(Source({ -1000.0f, 0.0f }, Color{ 255, 0, 0, 255 }));
        two.lights.push_back(Source({ 1000.0f, 0.0f }, Color{ 0, 0, 255, 255 }));
        const Render::Lighting::Sample s = two.At({ 0.0f, 0.0f });
        CHECK(s.tint.r > 100);
        CHECK(s.tint.b > 100);
        CHECK(s.tint.g < 20);
    }

    SUBCASE("only the strongest few are combined")
    {
        Render::Lighting many;
        for (int i = 0; i < 20; i++)
            many.lights.push_back(Source({ 0.0f, (float)(500 + i * 100) }, WHITE, 10000.0f, 0.1f));
        // Twenty tenth-strength lights would sum past full brightness if all counted.
        CHECK(many.At({ 0.0f, 0.0f }).strength < 1.0f);
    }
}

TEST_CASE("no lights means unlit, not black")
{
    const Render::Lighting none;
    CHECK(none.Empty());
    CHECK(none.At({ 0.0f, 0.0f }).strength == doctest::Approx(0.0f));
}

TEST_CASE("what lights a system comes from the archetypes in it")
{
    REQUIRE(Archetypes::Load(std::string(TEST_DATA_DIR) + "archetypes.json"));

    const Archetype* star = Archetypes::Find("star.yellow");
    REQUIRE(star != nullptr);
    CHECK(star->visual.lightRadius > 0.0f);

    const Archetype* hub = Archetypes::Find("station.trade_hub");
    REQUIRE(hub != nullptr);
    CHECK(hub->visual.lightRadius == doctest::Approx(0.0f));  // a station is not a light

    // An entity describes itself; what it emits rides on the item like everything else
    // about its look, so the thing that builds the light list needs no world.
    ::Star  sun({ 0.0f, 0.0f }, 600.0f, StarType::Yellow);
    Station hubEntity({ 500.0f, 0.0f }, 90.0f, "Hub", FactionId::Independent,
                      StationRole::TradeHub);

    std::vector<Render::Item> scene{ sun.Describe(), hubEntity.Describe() };
    const Render::Lighting    lg = Render::LightsFrom(scene);
    CHECK(lg.lights.size() == 1);  // the star, and not the station beside it
    CHECK(lg.lights[0].color.r == star->visual.color.r);
    CHECK(lg.lights[0].radius == doctest::Approx(star->visual.lightRadius));

    // The point of putting it in the archetype rather than testing for EntityKind::Star:
    // anything whose archetype says it glows is a light, including one a player invents.
    Render::Item beacon;
    beacon.pos = { 100.0f, 100.0f };
    beacon.lightRadius = 900.0f;
    beacon.lightIntensity = 0.5f;
    scene.push_back(beacon);
    CHECK(Render::LightsFrom(scene).lights.size() == 2);
}
