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

// Where the object a ship is holding station on is, and how fast it is going. The
// velocity is what lets a hold match a moving target instead of chasing the place it was a
// moment ago: all of it for a follow (#298) and an orbit, the part along the line of sight
// for a keep (#309).
struct HoldTarget
{
    Vector2 pos = { 0.0f, 0.0f };
    Vector2 vel = { 0.0f, 0.0f };
};

// Applies a client command to the ship: movement toggles, manual control (which cancels
// autopilot, warp and any standing hold), a navigation order, the piloting bonus, and the
// physics update for dt.
//
// `hold` is where the object the ship is holding station on currently is, or null if it is
// holding station on nothing. Both sides resolve it themselves -- the server from the live
// entity, the client from its interpolated proxy -- so the two will differ by roughly the
// render delay.
//
// That is deliberate and it is the one place this file tolerates a difference. A hold is a
// slow control loop aiming at a ring hundreds of units across; a tenth of a second of lag
// in where the ring is cannot be seen, and reconciliation removes what little there is. It
// would not be acceptable for anything that decides a hit, which is why nothing that
// decides a hit is computed from an interpolated position.
void StepPlayerShip(Ship& ship, const Proto::Command& cmd, float pilotBonus, float dt,
                    const HoldTarget* hold = nullptr);

// How far outside a station's hull a docked ship is berthed (#298).
inline constexpr float DOCK_BERTH_CLEARANCE = 60.0f;

// Where a ship docked at a station is: on the side it came in from, just clear of the
// hull. Docked means attached -- the station may be on an orbit (#210), and the ship goes
// round with it -- so this is recomputed from wherever the station is now, every tick
// while docked and once more at the undock. `bearing` is a unit vector from the station
// to the berth, fixed at the moment of docking.
Vector2 DockBerth(Vector2 stationPos, float stationSize, Vector2 bearing);

// The bearing a ship at `shipPos` docks on. Sitting exactly on the station's centre has
// no direction, so that one gets a fixed one rather than a division by zero.
Vector2 DockBearing(Vector2 stationPos, Vector2 shipPos);

}  // namespace Sim
