#pragma once

// Global scale parameters for a star system. A single source of truth for world
// dimensions, security rings, and the boundary — to avoid scattering magic numbers.
namespace World
{
// System radius: the ship is not allowed past this (a soft boundary). A million and not
// ten (#159): positions are float, and at 1e6 the gap between representable values is
// 0.06 of a unit; at 1e7 it is a whole one and a ship at the edge would jitter.
constexpr float SYSTEM_RADIUS = 1000000.0f;

// Beyond this from the star a belt is a pirate haunt (Simulation_Agents). Security itself
// comes from universe.json, not from a ring.
constexpr float MID_RADIUS = 775000.0f;

// A leg longer than this is crossed at warp by anything flying itself -- a standing order,
// an NPC. At sublight the next station is half an hour away (#159).
constexpr float WARP_WORTH_IT = 20000.0f;
}  // namespace World
