#pragma once

// Global scale parameters for a star system. A single source of truth for world
// dimensions, security rings, and the boundary — to avoid scattering magic numbers.
namespace World
{
// System radius: the ship is not allowed past this (a soft boundary).
constexpr float SYSTEM_RADIUS = 25000.0f;

// Security ring thresholds by distance from the star (center at {0,0}).
constexpr float CORE_RADIUS = 9000.0f;   // core: high security
constexpr float MID_RADIUS  = 18000.0f;  // middle ring: low security
// outskirts — from MID_RADIUS to SYSTEM_RADIUS: lawless
}
