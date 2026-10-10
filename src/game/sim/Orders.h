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
    Mine,    // approach a field and mine it
    Route,   // travel to another system, gate by gate
    // Holding station on an object at `stopDist` (#157, #298). These never finish on their
    // own: they run until replaced or aborted, or until the target is gone.
    Orbit,   // circle it
    Keep,    // keep at range
    Follow,  // keep at range and match its velocity
    // Close to weapon range of a ship or a structure and fire until it is gone (#39). The
    // weapon is armed for the order and disarmed when it ends; firing is the same server
    // verb a client's trigger drives, with the same consequences.
    Attack
};

inline bool IsHold(Kind k)
{
    return k == Kind::Orbit || k == Kind::Keep || k == Kind::Follow;
}

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
    float   stopDist = 120.0f;       // how close counts as arrived; a hold's range
    bool    useWarp = false;         // MoveTo: warp instead of cruising
    bool    untilFull = false;       // Mine: keep going until the hold is full

    // Route only. destSystem is where to end up; avoidDanger weighs a safer path over a
    // shorter one, which is the difference between a hauler and a courier that arrives.
    std::string destSystem;
    bool        avoidDanger = false;
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
