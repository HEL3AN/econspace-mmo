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

// The simulation tick. One client command is one of these on both sides, which is what
// makes prediction and its acknowledgements line up; Game.h used to hold a second copy
// with a comment warning that it had to match this one.
inline constexpr float SIM_DT = 1.0f / 60.0f;

// How much real time a client may make up in one frame, and therefore the largest burst
// of commands the server must accept without calling it a cheat (#115).
//
// Both sides need this number and they need the same one. The client clamps its
// accumulator to it so a stall cannot spiral into a thousand catch-up steps; the server
// derives its per-client tick budget from it, because one command is one tick of movement
// and refusing a legitimate burst makes an honest ship rubber-band. When the two disagreed
// -- a quarter second of client against eight ticks of server -- the game throttled every
// player on the first second of every session.
inline constexpr float MAX_CATCHUP_SECONDS = 0.25f;

// Applies a client command to the ship: movement toggles, manual control (which cancels
// autopilot and warp), a one-shot navigation order, the piloting bonus, and the physics
// update for dt.
void StepPlayerShip(Ship& ship, const Proto::Command& cmd, float pilotBonus, float dt);

}  // namespace Sim
