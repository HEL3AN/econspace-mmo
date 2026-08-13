#pragma once

#include "raylib.h"

#include <string>

// Standing orders: the strategic layer above the 60 Hz tick.
//
// The command path is per-tick by design -- one input, one step -- which suits a human
// hand on the keyboard and suits nothing else. An LLM agent thinks in seconds and costs
// money per decision, so it cannot stream thrust bits. An order is the unit it works in
// instead: "mine that belt until the hold is full", issued once, executed by the server
// over however long it takes, reported when it finishes or fails.
//
// The tactical loop underneath is untouched. An order does not move the ship itself; it
// decides what the command for this tick should be, and that command goes through the
// same Sim::StepPlayerShip a human client drives. There is one path into the ship.
//
// This is not only for agents. It is also the autopilot a human wants -- "fly there and
// dock" rather than holding a key and watching.
namespace Orders
{

enum class Kind
{
    None,
    MoveTo,  // fly to a point or an object and stop
    Dock,    // approach a station and dock with it
    Undock,  // leave the station
    Mine     // approach a field and mine it
};

enum class Status
{
    Idle,     // nothing ordered
    Running,  // in progress
    Done,     // finished as asked
    Failed    // could not finish; see detail
};

struct Order
{
    Kind    kind = Kind::None;
    int     targetId = 0;            // station / field / object to act on (0 — use point)
    Vector2 point = { 0.0f, 0.0f };  // destination for MoveTo without a target
    float   stopDist = 120.0f;       // how close counts as arrived
    bool    useWarp = false;         // MoveTo: warp instead of cruising
    bool    untilFull = false;       // Mine: keep going until the hold is full
};

// Hull fraction below which a running order gives up.
//
// This is part of the design, not a safety extra. An agent can stall for thirty seconds
// or crash outright, and an order that keeps flying a dying ship into the thing killing
// it turns agent play into a lottery. The order fails, the ship stops, and the agent is
// told why -- so the next decision is made with the facts.
constexpr float ABORT_HULL_FRACTION = 0.25f;

const char* KindName(Kind k);

}  // namespace Orders
