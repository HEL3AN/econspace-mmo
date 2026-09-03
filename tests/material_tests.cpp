#include <doctest/doctest.h>

#include "core/Archetype.h"
#include "render/Material.h"
#include "render/Scene.h"
#include <cstdio>
#include <fstream>
#include <string>

// A material is a shader plus what feeds it, and only the second half can be tested here:
// there is no graphics card on a CI runner, so what the shader *does* with a uniform is out
// of reach. What is in reach is which uniforms it gets and what they are worth, which is
// where a material silently draws the wrong thing.

namespace
{
Render::Binding Bound(const char* uniform, const char* source)
{
    Render::Binding b;
    b.uniform = uniform;
    REQUIRE(Render::SourceFromName(source, b.source));
    return b;
}
}  // namespace

TEST_CASE("every source is named the same way in data and in code")
{
    // The names are what a material file says, so a rename that misses one would turn a
    // binding into a load error rather than into anything visible.
    const char* names[] = { "item.color", "item.intensity", "item.heading",    "item.thrusting",
                            "item.size",  "item.screenPos", "item.screenSize", "light.dir",
                            "light.tint", "light.strength", "light.ambient",   "clock.time" };
    for (const char* n : names)
    {
        Render::Source s;
        INFO("source: ", n);
        REQUIRE(Render::SourceFromName(n, s));
        CHECK(std::string(Render::SourceName(s)) == n);
    }

    Render::Source unused;
    CHECK_FALSE(Render::SourceFromName("item.mood", unused));
}

TEST_CASE("a binding is worth what the object and the scene say")
{
    Render::Item item;
    item.color = { 255, 128, 0, 255 };
    item.intensity = 0.25f;
    item.heading = 1.5f;
    item.thrusting = true;
    item.size = 90.0f;

    Render::MaterialInputs in;
    in.item = &item;
    in.ambient = 0.38f;
    in.light.dir = { -1.0f, 0.0f };
    in.light.strength = 0.7f;
    in.light.tint = { 255, 244, 214, 255 };
    in.screenPos = { 640.0f, 360.0f };
    in.screenSize = 42.0f;
    in.time = 12.5f;

    SUBCASE("colours arrive as 0..1, because that is what a shader reads")
    {
        const Render::UniformValue u = Render::Resolve(Bound("base", "item.color"), in);
        CHECK(u.count == 4);
        CHECK(u.v[0] == doctest::Approx(1.0f));
        CHECK(u.v[1] == doctest::Approx(128.0f / 255.0f));
        CHECK(u.v[2] == doctest::Approx(0.0f));
    }

    SUBCASE("a flag arrives as a number, since a shader has no bool uniform worth using")
    {
        CHECK(Render::Resolve(Bound("burn", "item.thrusting"), in).v[0] == doctest::Approx(1.0f));
        item.thrusting = false;
        CHECK(Render::Resolve(Bound("burn", "item.thrusting"), in).v[0] == doctest::Approx(0.0f));
    }

    SUBCASE("the scene's own values, not the item's")
    {
        CHECK(Render::Resolve(Bound("a", "light.ambient"), in).v[0] == doctest::Approx(0.38f));
        CHECK(Render::Resolve(Bound("s", "light.strength"), in).v[0] == doctest::Approx(0.7f));
        const Render::UniformValue d = Render::Resolve(Bound("d", "light.dir"), in);
        CHECK(d.count == 2);
        CHECK(d.v[0] == doctest::Approx(-1.0f));
        CHECK(Render::Resolve(Bound("t", "clock.time"), in).v[0] == doctest::Approx(12.5f));
    }

    SUBCASE("where the object landed on screen, which is the camera's answer and not the item's")
    {
        const Render::UniformValue c = Render::Resolve(Bound("centre", "item.screenPos"), in);
        CHECK(c.count == 2);
        CHECK(c.v[0] == doctest::Approx(640.0f));
        CHECK(c.v[1] == doctest::Approx(360.0f));
        CHECK(Render::Resolve(Bound("r", "item.screenSize"), in).v[0] == doctest::Approx(42.0f));
    }

    SUBCASE("a material on an object it was not given draws wrong rather than crashing")
    {
        Render::MaterialInputs none;  // no item at all
        CHECK(Render::Resolve(Bound("d", "item.intensity"), none).v[0] == doctest::Approx(0.0f));
        CHECK(Render::Resolve(Bound("c", "item.color"), none).count == 4);
    }
}

TEST_CASE("a constant in the data is a constant in the shader")
{
    REQUIRE(Render::Materials::Load(std::string(TEST_DATA_DIR) + "materials.json"));

    Render::Binding b;
    b.uniform = "grit";
    b.source = Render::Source::Constant;
    b.constant.count = 3;
    b.constant.v[0] = 0.1f;
    b.constant.v[1] = 0.2f;
    b.constant.v[2] = 0.3f;

    Render::MaterialInputs     in;
    const Render::UniformValue u = Render::Resolve(b, in);
    CHECK(u.count == 3);
    CHECK(u.v[1] == doctest::Approx(0.2f));
}

TEST_CASE("a material naming a source that does not exist is refused, not quietly emptied")
{
    // Deliberately unlike the screen treatment, where an unknown pass is skipped. Losing a
    // pass makes the picture plainer; losing a uniform makes a material draw something
    // actively wrong, and it would do it in silence.
    const std::string bad = std::string(TEST_DATA_DIR) + "../build/bad_material.json";
    {
        std::ofstream out(bad);
        REQUIRE(out.is_open());
        out << R"({ "materials": [ { "id": "x", "bindings": { "u": "item.mood" } } ] })";
    }
    CHECK_FALSE(Render::Materials::Load(bad));
    CHECK(Render::Materials::Error().find("item.mood") != std::string::npos);
    std::remove(bad.c_str());

    // And the previous contents survive a failed load, so a bad edit does not empty the
    // registry the running game is drawing from.
    REQUIRE(Render::Materials::Load(std::string(TEST_DATA_DIR) + "materials.json"));
}

TEST_CASE("the materials that ship are the ones the archetypes ask for")
{
    REQUIRE(Render::Materials::Load(std::string(TEST_DATA_DIR) + "materials.json"));
    REQUIRE(Archetypes::Load(std::string(TEST_DATA_DIR) + "archetypes.json"));

    int wearing = 0;
    for (const Archetype& a : Archetypes::All())
    {
        if (a.visual.material.empty())
            continue;
        wearing++;
        INFO("archetype: ", a.id);
        // A typo here is silent: the object just draws plain, which is exactly what it did
        // before materials existed.
        CHECK(Render::Materials::Find(a.visual.material) != nullptr);
    }
    CHECK(wearing > 0);

    SUBCASE("a star wears none, because it is the light rather than something lit")
    {
        const Archetype* star = Archetypes::Find("star.yellow");
        REQUIRE(star != nullptr);
        CHECK(star->visual.material.empty());
    }

    SUBCASE("the material reaches the item, which is all the backend ever sees")
    {
        const Archetype* hub = Archetypes::Find("station.trade_hub");
        REQUIRE(hub != nullptr);
        CHECK(Render::FromArchetype(*hub, { 0.0f, 0.0f }, 90.0f).material == hub->visual.material);
    }
}
