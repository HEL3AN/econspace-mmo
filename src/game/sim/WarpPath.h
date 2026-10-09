#pragma once

#include "raylib.h"
#include <vector>

// The way a warp goes around a body instead of through it (#160).
//
// A warp is a straight line, and in a million-unit system a straight line between two
// stations on either side of the star goes through the star: the screen fills with yellow
// for a second at a quarter of a million units a second. The line is bent once, at a point
// beside the body, chosen by whoever gives the order -- the client from its layout, the
// server from its entities -- and carried in the command. The ship only follows it, so the
// prediction stays exact: both sides fly the same two legs because both were given them.
namespace WarpPath
{

struct Body
{
    Vector2 pos;
    float   radius;
};

// How far outside a body's surface a warp passes, as a share of its radius.
constexpr float CLEARANCE = 0.25f;

// If the straight line from `from` to `to` passes through a body (with clearance), sets
// `via` to a point beside the largest such body from which both legs are clear of it, and
// returns true. Returns false when the line is clear -- or when one end is itself inside a
// body, where no detour helps and the warp goes as ordered.
bool Via(Vector2 from, Vector2 to, const std::vector<Body>& bodies, Vector2& via);

// The nearest the segment from `a` to `b` comes to `p`.
float SegmentDistance(Vector2 a, Vector2 b, Vector2 p);

}  // namespace WarpPath
