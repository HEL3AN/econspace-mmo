#pragma once

#include <string>
#include <vector>

// What to do next, in one line (#364): the prompt a new or idle player sees under the
// location plate, with a pointer at what it names.
//
// It is worked out from where the player is and what they have taken on, never scripted:
// a tutorial that walks through steps in order is wrong the moment a player does one out of
// order, and this one cannot be, because it has no order -- it reads the state again every
// frame. No drawing and no Game here, so a test can hold every rule.
namespace Goal
{

// One mission the player has taken, as the missions window sees it (MissionList::Row).
struct Mission
{
    std::string title;
    bool        ready = false;  // can be handed in now: the server says so
    std::string needs;          // what it still needs, in the agent's words; empty when ready
    int         handInId = 0;   // the station it is handed in at
    std::string handInName;     // ...its name, when it is in this system; empty otherwise
};

struct State
{
    bool                 docked = false;
    int                  dockedStationId = 0;
    int                  offers = 0;            // on the docked station's board
    bool                 roomForMore = true;    // fewer missions taken than the most one may have
    std::vector<Mission> active;                // in the order the server lists them
    int                  nearestStationId = 0;  // the nearest station in this system, 0 if none
    std::string          nearestStationName;
};

enum class Step
{
    None,
    DockForWork,  // in space with nothing to do: the nearest station has a job board
    FindStation,  // ...and this system has no station
    TakeMission,  // docked at a board with work on it
    HandIn,       // docked where a finished mission is handed in
    Undock,       // docked with work to do elsewhere, or with nothing here
    FlyTo,        // a mission is finished, and the station it goes back to is in this system
    Work,         // a mission is under way: what it still needs
    FarHandIn,    // a mission is finished and goes back to another system
};

struct Prompt
{
    Step        step = Step::None;
    std::string text;
    // What the pointer points at: a thing in this system by id, or the missions window.
    int  pointAt = 0;
    bool missionsWindow = false;
};

Prompt Next(const State& state);

// The prompt is for learning the loop -- dock, take work, do it, hand it in -- and goes away
// once the player has been round it this many times, or turns it off in Settings.
constexpr int LEARNED_AFTER = 3;
bool          Shown(bool setting, int handedIn);

}  // namespace Goal
