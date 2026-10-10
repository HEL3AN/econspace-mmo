#include <doctest/doctest.h>

#include "entities/Ship.h"
#include "entities/ShipType.h"
#include "sim/PlayerStep.h"
#include "sim/WarpPath.h"
#include <algorithm>
#include <cmath>

// Warp across a million-unit system (#160). Nothing about it needs a window, and before
// this file nothing about it was tested at all.

namespace
{
struct WarpRun
{
    float seconds = 0.0f;   // from the order to the drop-out, alignment included
    float peak = 0.0f;      // the fastest it went
    float closest = 1e30f;  // the nearest it came to the target
    bool  arrived = false;
};

WarpRun Warp(float distance, float drop)
{
    Ship        ship({ 0.0f, 0.0f }, GetShipCatalog()[0].stats);
    const float dt = Sim::SIM_DT;
    ship.EngageWarp({ distance, 0.0f }, drop);
    WarpRun run;
    for (int i = 0; i < 60 * 120 && ship.IsWarping(); i++)
    {
        ship.Update(dt);
        run.seconds += dt;
        run.peak = std::max(run.peak, ship.GetSpeed());
        run.closest = std::min(run.closest, distance - ship.GetPosition().x);
    }
    run.arrived = !ship.IsWarping();
    return run;
}
}  // namespace

TEST_CASE("a warp ends at its drop distance, stopped, and never overshoots it")
{
    for (float d : { 25000.0f, 300000.0f, 900000.0f })
    {
        CAPTURE(d);
        const WarpRun run = Warp(d, 500.0f);
        REQUIRE(run.arrived);
        // Overshooting the drop-out by one fast tick is thousands of units: past a gate,
        // through a station.
        CHECK(run.closest >= 500.0f - 1.0f);
        CHECK(run.closest <= 500.0f + 1.0f);
        CHECK(run.peak <= Ship::WARP_MAX_SPEED + 1.0f);
    }
}

TEST_CASE("a warp that has arrived has ended: never still warping at a standstill (#259)")
{
    // The HUD was seen saying WARP at speed 0 after an arrival. Far from the origin a float
    // cannot land exactly on the drop-out point, so the last step must not leave a warp
    // creeping the remaining fraction of a unit at a speed of nothing. The one tick between
    // the alignment ending and the first push is the only standstill a warp has.
    const float dt = Sim::SIM_DT;
    for (Vector2 from : { Vector2{ -574800.0f, 143700.0f }, Vector2{ 600000.0f, 377800.0f },
                          Vector2{ 0.0f, -900000.0f }, Vector2{ 1234.5f, 0.0f } })
        for (float drop : { 500.0f, 2500.0f, 15000.0f })
        {
            CAPTURE(from.x);
            CAPTURE(drop);
            Ship ship(from, GetShipCatalog()[0].stats);
            ship.EngageWarp({ 948000.0f, 0.0f }, drop);
            int  still = 0;
            bool moved = false;
            for (int i = 0; i < 60 * 120 && ship.IsWarping(); i++)
            {
                ship.Update(dt);
                moved = moved || ship.GetSpeed() > 0.0f;
                if (moved && ship.IsWarping() && ship.GetSpeed() < 1.0f)
                    still++;
            }
            CHECK_FALSE(ship.IsWarping());
            CHECK(still == 0);
        }
}

TEST_CASE("how far a warp goes costs little time, so a system is one place")
{
    // At a flat speed, crossing the system took forty-five times as long as a hop between
    // neighbours. Accelerating and slowing in proportion makes it a few seconds either
    // way, which is the whole of #160.
    const WarpRun hop = Warp(20000.0f, 500.0f);
    const WarpRun across = Warp(900000.0f, 500.0f);
    REQUIRE(hop.arrived);
    REQUIRE(across.arrived);
    MESSAGE("hop " << hop.seconds << " s, across " << across.seconds << " s");
    CHECK(hop.seconds > Ship::WARP_ALIGN_TIME + 1.0f);  // still a journey, not a blink
    CHECK(across.seconds < 15.0f);
    CHECK(across.seconds < hop.seconds * 3.0f);
}

TEST_CASE("a warp picked up mid-flight from a snapshot continues exactly as the original")
{
    // Prediction (#157, PlayerStep.h): the client takes the server's position, velocity and
    // warp state from a snapshot and steps on from there. If the warp needed anything the
    // snapshot does not carry, the client would compute a different flight and every
    // correction would be a visible jump at a hundred thousand units a second.
    const float dt = Sim::SIM_DT;
    Ship        server({ 0.0f, 0.0f }, GetShipCatalog()[0].stats);
    // Bent around a star, and picked up during the first leg: the bend is part of what a
    // snapshot has to carry.
    const std::vector<WarpPath::Body> star = { { { 300000.0f, 0.0f }, 150000.0f } };
    Vector2                           via{};
    REQUIRE(WarpPath::Via({ 0.0f, 0.0f }, { 600000.0f, 0.0f }, star, via));
    server.EngageWarp({ 600000.0f, 0.0f }, 500.0f, true, via);
    for (int i = 0; i < 60 * 4; i++)  // through the alignment and into the climb
        server.Update(dt);
    REQUIRE(server.GetWarpPhase() == WarpPhase::Warping);
    REQUIRE(server.HasWarpVia());

    Ship client({ 0.0f, 0.0f }, GetShipCatalog()[0].stats);
    client.ApplyView(server.GetPosition(), server.GetHeading(), server.GetVelocity(), 100.0f,
                     100.0f);
    client.ApplyNavView((int)server.GetWarpPhase(), server.GetWarpAlignTimer(),
                        server.GetWarpTarget(), server.GetWarpDrop(), server.HasWarpVia(),
                        server.GetWarpVia(), false, { 0, 0 }, 0.0f, 0, 0, 0.0f);

    for (int i = 0; i < 60 * 3; i++)
    {
        server.Update(dt);
        client.Update(dt);
    }
    CHECK(client.GetPosition().x == server.GetPosition().x);
    CHECK(client.GetPosition().y == server.GetPosition().y);
    CHECK(client.GetSpeed() == server.GetSpeed());
}

TEST_CASE("a warp bent around the star never enters it, and arrives (#160)")
{
    // Two stations on either side of the star: the straight line is through it.
    const std::vector<WarpPath::Body> star = { { { 0.0f, 0.0f }, 150000.0f } };
    const Vector2                     from = { 320000.0f, 0.0f }, to = { -600000.0f, 0.0f };
    Vector2                           via{};
    REQUIRE(WarpPath::Via(from, to, star, via));

    Ship ship(from, GetShipCatalog()[0].stats);
    ship.EngageWarp(to, 500.0f, true, via);
    float nearest = 1e30f, seconds = 0.0f;
    for (int i = 0; i < 60 * 60 && ship.IsWarping(); i++)
    {
        ship.Update(Sim::SIM_DT);
        seconds += Sim::SIM_DT;
        const Vector2 p = ship.GetPosition();
        nearest = std::min(nearest, std::sqrt(p.x * p.x + p.y * p.y));
    }
    REQUIRE_FALSE(ship.IsWarping());
    CHECK(nearest >= 150000.0f);
    const Vector2 p = ship.GetPosition();
    CHECK(std::sqrt((p.x - to.x) * (p.x - to.x) + (p.y - to.y) * (p.y - to.y)) ==
          doctest::Approx(500.0f).epsilon(0.01));
    CHECK(seconds < 20.0f);
}
