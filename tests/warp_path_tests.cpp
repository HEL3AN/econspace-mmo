#include <doctest/doctest.h>

#include "sim/WarpPath.h"
#include <cmath>

// Where a warp bends to go around a body (#160).

namespace
{
const std::vector<WarpPath::Body> STAR = { { { 0.0f, 0.0f }, 150000.0f } };
}

TEST_CASE("a clear line is not bent")
{
    Vector2 via{};
    CHECK_FALSE(WarpPath::Via({ 300000.0f, 400000.0f }, { -300000.0f, 400000.0f }, STAR, via));
}

TEST_CASE("a line through a star is bent beside it, and both legs clear it")
{
    const Vector2 from = { 400000.0f, 10000.0f }, to = { -600000.0f, -20000.0f };
    Vector2       via{};
    REQUIRE(WarpPath::Via(from, to, STAR, via));
    CHECK(WarpPath::SegmentDistance(from, via, STAR[0].pos) >= STAR[0].radius);
    CHECK(WarpPath::SegmentDistance(via, to, STAR[0].pos) >= STAR[0].radius);
}

TEST_CASE("the bend is on the side the line already passes, and a dead-centre line always turns "
          "the same way")
{
    Vector2 via{};
    // Passing a little north of the centre (y down, so north is negative y): bend north.
    REQUIRE(WarpPath::Via({ 400000.0f, -20000.0f }, { -400000.0f, -20000.0f }, STAR, via));
    CHECK(via.y < 0.0f);

    // Straight through the middle: no side to prefer, so a fixed one -- the same answer on
    // a client and a server, every time.
    Vector2 a{}, b{};
    REQUIRE(WarpPath::Via({ 400000.0f, 0.0f }, { -400000.0f, 0.0f }, STAR, a));
    REQUIRE(WarpPath::Via({ 400000.0f, 0.0f }, { -400000.0f, 0.0f }, STAR, b));
    CHECK(a.x == b.x);
    CHECK(a.y == b.y);
}

TEST_CASE("the largest body in the way decides the bend")
{
    const std::vector<WarpPath::Body> bodies = { { { 200000.0f, 0.0f }, 15000.0f },
                                                 { { 0.0f, 0.0f }, 150000.0f } };
    Vector2                           via{};
    REQUIRE(WarpPath::Via({ 400000.0f, 1000.0f }, { -400000.0f, 1000.0f }, bodies, via));
    // Beside the star, not beside the planet.
    CHECK(std::abs(via.x) < 150000.0f);
    CHECK(std::abs(via.y) >= 150000.0f);
}

TEST_CASE("a warp that starts or ends at a body is left alone")
{
    // Leaving a planet's surface or warping to a star: no detour reaches either end.
    Vector2 via{};
    CHECK_FALSE(WarpPath::Via({ 100000.0f, 0.0f }, { -400000.0f, 0.0f }, STAR, via));
    CHECK_FALSE(WarpPath::Via({ 400000.0f, 0.0f }, { 0.0f, 0.0f }, STAR, via));
}
