#pragma once

#include "core/Faction.h"
#include "sim/Observation.h"

#include <string>
#include <vector>

// The missions window as a list (#297): which missions, and what each says about itself.
//
// The half of the window with no drawing in it, as Overview is for the overview. It reads
// what the client was sent -- the snapshot's offers and active missions, the layout's
// stations -- through the same Obs::View the agent's text is built from, so what a mission
// "still needs" is the same phrase in the window and in the agent's missions tool.
namespace MissionList
{

enum class Board
{
    Active,  // the missions taken, carried across systems until handed in
    Offers,  // the board of the station the ship is docked at
};

struct Row
{
    // Its number in the snapshot's list: what accept_mission and complete_mission take.
    int         index = 0;
    std::string title;
    std::string description;
    std::string kind;                              // "bounty", "mining", "delivery"
    std::string status;                            // how far along: "2 / 5", "ready", "deliver"
    FactionId   faction = FactionId::Independent;  // who gave it
    double      reward = 0.0;
    float       rep = 0.0f;
    // How much of a count is done, 0..1; negative when there is nothing to count (a
    // delivery is the trip).
    float       progress = -1.0f;
    bool        ready = false;  // can be handed in now: the server says so
    std::string needs;          // what it still needs, empty when ready (Obs::MissionNeeds)
    int         handInId = 0;   // the station it is handed in at
    // That station's name, and whether it is in this system -- somewhere to fly to now.
    // Empty and false when it is in another one: the client sees only its own system.
    std::string handInName;
    bool        handInHere = false;
};

std::vector<Row> Build(const Obs::View& view, Board board);

// "bounty", "mining", "delivery".
const char* KindWord(int missionType);

}  // namespace MissionList
