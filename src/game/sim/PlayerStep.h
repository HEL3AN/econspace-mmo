#pragma once

#include "sim/Protocol.h"

class Ship;

// The player ship's physical step, and the constants that go with it.
//
// This lives in one place because BOTH sides run it: the server applies it
// authoritatively, and the client applies the identical step to predict its own ship
// and to replay unacknowledged inputs on top of an authoritative snapshot. Prediction
// only works while the two compute the same result from the same input, so there must
// be exactly one implementation — not one per side that has to be kept in sync by hand.
namespace Sim
{

// Player weapon range. Shared for the same reason: the server decides whether a shot
// connects, the client draws the targeting circle, and the two must not disagree.
inline constexpr float PLAYER_WEAPON_RANGE = 280.0f;

// Applies a client command to the ship: movement toggles, manual control (which cancels
// autopilot and warp), a one-shot navigation order, the piloting bonus, and the physics
// update for dt.
void StepPlayerShip(Ship& ship, const Proto::Command& cmd, float pilotBonus, float dt);

}  // namespace Sim
