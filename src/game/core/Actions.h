#pragma once

#include "raylib.h"  // Vector2: a plain struct, nothing here draws
#include "core/Blueprint.h"
#include "entities/EntityKind.h"

#include <string>
#include <vector>

// What a player can do about a thing, as one list (#297).
//
// The context menu, the selected-item window and the overview's right click all offer the
// actions for an object, and before this each built its own -- so they drifted, and the
// target window offered none at all. Now there is one function, For, and every place that
// offers an action reads it. Each action also names the econagent tool that does the same
// thing, so the design rule that a button cannot exist without a matching tool for an agent
// (#42) is a property a test can check rather than a hope; the few that have no tool yet
// say so (`tool` empty) and the test keeps that list honest.
//
// No raylib drawing and no Game here: what to offer is decided from what the client sees
// of the thing, and Game::Perform turns a chosen action into a command.
namespace Actions
{

enum class Verb
{
    Select,       // make it the target: the selected-item window shows it
    Approach,     // fly to it at sublight and stop beside it
    Orbit,        // the standing behaviours (#157), at a range
    Keep,         // ...hold this distance from it
    Follow,       // ...hold the distance and match its velocity (#298)
    SetRange,     // choose the range: opens the selected-item window's range control
    Warp,         // warp to it, dropping out at `distance`
    Dock,         // a station
    Mine,         // a belt
    Attack,       // a ship, or a structure that is not your own (#39)
    Investigate,  // a wreck
    Jump,         // a gate
    FlyHere,      // a point in space
    WarpHere,
    Build,      // lay down a blueprint at a point (#39)
    Accept,     // take an offer from the board of the station the ship is docked at
    HandIn,     // hand in an active mission, for its reward
    Dismantle,  // take one's own structure apart for a refund (#39)
};

struct Action
{
    Verb        verb = Verb::Select;
    std::string label;            // what the menu says
    float       distance = 0.0f;  // the hold's range, the approach's stop, the warp's drop
    std::string blueprint;        // Build: which one
    int         mission = -1;     // Accept, HandIn: the offer's or the mission's number
    std::string tool;             // the econagent tool that does the same; empty if none yet
};

// What the client knows about a thing it might act on.
struct Target
{
    int         id = 0;
    EntityKind  kind = EntityKind::Unknown;
    Vector2     pos{ 0.0f, 0.0f };
    float       size = 0.0f;  // its radius
    std::string name;
    bool        looted = false;   // a wreck already searched
    std::string destinationName;  // a gate: where it leads, if the galaxy index knows
    // A structure: whether the viewer built it, and what dismantling it would return now
    // ("5 Iron, 1 Crystal"; empty when that is not known).
    bool        mine = false;
    std::string refund;
};

// The distances a hold is offered at, as multiples of the target's own radius: the same
// menu is then sensible beside a sixteen-unit ship and a station forty times larger, and
// survives the system growing (M9).
constexpr float HOLD_RANGES[] = { 2.0f, 5.0f, 12.0f };
// What a range control starts at for a thing of this radius.
float DefaultRange(float size);
// Beyond this, a warp is offered as well as an approach.
constexpr float WARP_MIN = 1800.0f;

// The actions for a thing, seen from `from` (the ship), in the order a menu lists them:
// the common ones, the standing behaviours, the warps, then what this kind of thing offers.
std::vector<Action> For(const Target& target, Vector2 from);

// The actions for an empty point in space: fly, warp, and whatever can be built there.
std::vector<Action> ForPoint(Vector2 point, Vector2 from, const std::vector<Blueprint>& blueprints);

// What the client knows about a mission it might act on. `index` is its number in the
// snapshot's list -- the number accept_mission and complete_mission take.
struct MissionTarget
{
    int  index = -1;
    bool offer = false;   // on the board, not yet taken
    bool ready = false;   // can be handed in now: the server says so
    bool docked = false;  // the job board is a station's
    int  active = 0;      // missions already taken
};

// What can be done with a mission itself: take it, or hand it in. Where it is handed in is
// a station, and what can be done about that is For(station).
std::vector<Action> ForMission(const MissionTarget& mission);

// The econagent tool that does what a verb does; empty for one no tool covers yet.
const char* ToolFor(Verb verb);

// The navMode a standing behaviour is sent as (3 orbit, 4 keep, 5 follow); 0 for any other.
int HoldMode(Verb verb);

}  // namespace Actions
