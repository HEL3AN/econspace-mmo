#include <doctest/doctest.h>

#include "core/Archetype.h"
#include "core/Blueprint.h"
#include "core/Faction.h"
#include "core/Paint.h"
#include "entities/ShipType.h"
#include "render/Material.h"
#include "sim/ClientSession.h"
#include "sim/Simulation.h"
#include "temp_dir.h"

#include <nlohmann/json.hpp>

#include <fstream>
#include <string>

// A pilot's own paint on a palette livery (#552): the rules, the server keeping only what they
// allow, a save that survives damage, and the renderer's bounded cache.

namespace
{
const std::string DATA = TEST_DATA_DIR;

struct World
{
    Simulation     sim;
    ClientSession* s = nullptr;
    World()
    {
        REQUIRE(Factions::Load(DATA + "factions.json"));
        REQUIRE(Archetypes::Load(DATA + "archetypes.json"));
        REQUIRE(Blueprints::Load(DATA + "blueprints.json"));
        sim.LoadUniverse(std::string(TEST_WORLD_DIR) + "universe.json");
        sim.Seed(1234u);
        sim.InitGalaxy();
        sim.MaterializeAllSystems(std::string(TEST_WORLD_DIR) + "systems/");
        s = &sim.CreateSession(sim.Universe().startId, Vector2{ 0.0f, 0.0f },
                               GetShipCatalog()[0].stats);
        s->accountName = "painter";
    }
};
}  // namespace

TEST_CASE("every cell of the grid reads back as itself, and a hull is never loud")
{
    for (Paint::Role role : { Paint::Role::Base, Paint::Role::Band })
        for (int h = 0; h < Paint::HUES; h++)
            for (int l = 0; l < Paint::LIGHTS; l++)
                for (int s = 0; s < Paint::SATS; s++)
                {
                    const Paint::Cell c{ h, l, s };
                    const Paint::Rgb  rgb = Paint::RgbOf(role, c);
                    const Paint::Cell back = Paint::Nearest(role, rgb);
                    CHECK(Paint::RgbOf(role, back) == rgb);
                }
}

TEST_CASE("a band that does not contrast with the hull is moved until it does")
{
    Paint::Paint p;
    p.base = { 3, 2, 1 };
    p.band = { 3, 2, 1 };  // as light as the hull
    CHECK_FALSE(Paint::Contrasts(p.base, p.band));
    const Paint::Paint snapped = Paint::Snap(p);
    CHECK(Paint::Contrasts(snapped.base, snapped.band));
    CHECK(snapped.band.h == 3);  // the colour is the pilot's; only its lightness moved
}

TEST_CASE("a pattern is never flat and a light stays on its scale")
{
    Paint::Paint p;
    p.pattern = 0;
    p.light = 99;
    p.band = { 0, 4, 2 };
    p.base = { 0, 0, 0 };
    p = Paint::Snap(p);
    CHECK(p.pattern == Paint::PATTERN_MIN);
    CHECK(p.light == Paint::LIGHT_MAX);
}

TEST_CASE("the server keeps the canonical spec of what it is asked for (#552)")
{
    World w;
    CHECK(w.sim.SetLivery(*w.s, "guild"));
    CHECK(w.s->livery == "guild");

    // A colour off the grid, a flat pattern, a band as pale as the hull.
    CHECK(w.sim.SetLivery(*w.s, "guild/f0f0f0f1f1f1p0l9"));
    const std::string kept = w.s->livery;
    std::string       palette;
    bool              painted = false;
    Paint::Paint      p;
    REQUIRE(Paint::Split(kept, palette, painted, p));
    CHECK(palette == "guild");
    CHECK(painted);
    CHECK(p.pattern == Paint::PATTERN_MIN);
    CHECK(p.light == Paint::LIGHT_MAX);
    CHECK(Paint::Contrasts(p.base, p.band));
    // Asked again, it is a fixed point.
    CHECK(w.sim.SetLivery(*w.s, kept));
    CHECK(w.s->livery == kept);

    CHECK(w.sim.SetLivery(*w.s, "default"));
    CHECK(w.s->livery.empty());
}

TEST_CASE("a spec that does not parse is refused, never quietly reset (#552)")
{
    World w;
    REQUIRE(w.sim.SetLivery(*w.s, "sect"));
    CHECK_FALSE(w.sim.SetLivery(*w.s, "sect/nothex"));
    CHECK_FALSE(w.sim.SetLivery(*w.s, "sect/zzzzzzzzzzzzp1l1"));
    CHECK_FALSE(w.sim.SetLivery(*w.s, "Sect"));
    CHECK(w.s->livery == "sect");
}

TEST_CASE("a damaged paint in a save costs the paint and keeps the palette (#552)")
{
    World             w;
    const TempDir     tmp;
    const std::string path = tmp.Path("painter.json");
    REQUIRE(w.sim.SetLivery(*w.s, "guild/c8b89a2f5fa6p2l1"));
    REQUIRE(w.sim.SaveAccount(*w.s, path));
    const std::string good = w.s->livery;

    {
        ClientSession& again = w.sim.CreateSession(w.sim.Universe().startId, Vector2{ 0.0f, 0.0f },
                                                   GetShipCatalog()[0].stats);
        again.accountName = "painter";
        REQUIRE(w.sim.LoadAccount(again, path) == Save::Result::Ok);
        CHECK(again.livery == good);
    }

    nlohmann::json j;
    {
        std::ifstream in(path);
        in >> j;
    }
    j["livery"] = "guild/garbage";
    {
        std::ofstream out(path);
        out << j.dump();
    }
    ClientSession& damaged = w.sim.CreateSession(w.sim.Universe().startId, Vector2{ 0.0f, 0.0f },
                                                 GetShipCatalog()[0].stats);
    damaged.accountName = "painter";
    REQUIRE(w.sim.LoadAccount(damaged, path) == Save::Result::Ok);
    CHECK(damaged.livery == "guild");
}

TEST_CASE("the renderer resolves a spec into the palette livery with the paint on it (#552)")
{
    REQUIRE(Render::Materials::Load(DATA + "materials.json"));
    Paint::Paint p;
    p.base = { 5, 3, 1 };
    p.band = { 12, 1, 2 };
    p.pattern = 2;
    p.light = 3;
    const std::string spec = Paint::Format("guild", p);
    const auto*       l = Render::Materials::FindLivery(spec);
    REQUIRE(l != nullptr);
    CHECK(l->id == spec);
    const Paint::Rgb band = Paint::RgbOf(Paint::Role::Band, p.band);
    CHECK(l->accent.r == band[0]);
    CHECK(l->params[2] == doctest::Approx(2.0f));
    // The structure is the palette's own.
    const auto* guild = Render::Materials::FindLivery("guild");
    CHECK(l->params[0] == doctest::Approx(guild->params[0]));
    CHECK(l->style[3] == doctest::Approx(guild->style[3]));

    // A crowd of distinct paints is bounded, and a spec still resolves after it.
    for (int i = 0; i < 2000; i++)
    {
        Paint::Paint q = p;
        q.base = { i % 24, (i / 24) % 5, (i / 120) % 3 };
        q.band = { (i * 7) % 24, 4, 2 };
        REQUIRE(Render::Materials::FindLivery(Paint::Format("guild", Paint::Snap(q))) != nullptr);
    }
    CHECK(Render::Materials::FindLivery(spec)->id == spec);

    // An unknown palette draws as the default livery, as a bare unknown id does.
    CHECK(Render::Materials::FindLivery(Paint::Format("nowhere", p))->id == "default");
}
