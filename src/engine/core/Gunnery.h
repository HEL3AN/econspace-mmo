#pragma once

#include "core/ShipDesign.h"

#include <vector>

// The one hit rule (#619 slice 2, documents/combat_and_flight.md 3.2), read by the server for
// every shot -- a pilot's, an NPC's -- and by the client for the figure on the target card.
// No dice: quality scales the damage, so a test can hold it and an agent can reason about it.
//
//   range term    = 0.5 ^ ( max(0, d - optimal) / falloff )
//   tracking term = 0.5 ^ ( (omega / tracking) * (signatureRes / signature) ) ^ 2
//   quality       = range term * tracking term         (0..1)
//
// `omega` is the rate the target crosses the gun's line of sight, from the two ships' positions
// and velocities -- the server's, never an interpolated proxy's (CLAUDE.md, #157). It is
// scale-free: a long gun tracks slowly and wants distance, a short one tracks fast and wants
// the brawl, and the same rule holds for two frigates and for a fleet.
//
// Sieges, cold fights and patrols reckon damage at quality 1 (Ships::Dps): a stationary target
// at optimal range. A live fight is worse than that reference, never better.
namespace Ships
{
// How far past `optimal` a gun still hits at all: its effect is an eighth there.
inline constexpr float REACH_FALLOFFS = 3.0f;

// The angular velocity of a target at (dx, dy) from the shooter, moving at (dvx, dvy) relative
// to it, in radians a second: the transversal part of the relative velocity over the distance.
float AngularVelocity(float dx, float dy, float dvx, float dvy);

// One gun's quality against a target `distance` away crossing at `omega`, of `signature`.
float HitQuality(const GunSpec& gun, float distance, float omega, float signature);

// Where a gun stops hitting: its optimal plus REACH_FALLOFFS falloffs.
inline float ReachOf(const GunSpec& g)
{
    return g.optimal + REACH_FALLOFFS * g.falloff;
}

// A shot of all of a design's guns: the damage that gets through (the sum of each gun's damage
// times its own quality), what it is made of (a share per damage type of that damage, the rest
// untyped) and the shot's quality as a whole -- that damage over the reference volley.
struct Shot
{
    float      damage = 0.0f;
    TypeValues mix{};
    float      quality = 0.0f;
};
Shot Gunnery(const std::vector<Gun>& guns, float distance, float omega, float signature);

// What a ship's guns amount to as one: the damage-weighted optimal and falloff (the rings a
// pilot sees) and the longest reach of any gun that does damage.
struct Profile
{
    float optimal = 0.0f;
    float falloff = 0.0f;
    float reach = 0.0f;
};
Profile ProfileOf(const std::vector<Gun>& guns);
}  // namespace Ships
