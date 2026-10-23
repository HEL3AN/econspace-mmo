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
    // Its place in the snapshot's list: what complete_mission takes for an active mission.
    int index = 0;
    // An offer's number (#425), which stays the same while the board changes around it: what
    // accept_mission takes. 0 for an active mission.
    int         offerId = 0;
    std::string title;
    std::string description;
    // The title and what tells it from another of its kind, short enough for a row (#427):
    // "Delivery to Vela Yards", "Bounty: 3 pirates", "Supply: 20 Steel, Kelos". A board of
    // three rows that all read "Delivery" asks for a click on each to tell them apart.
    std::string summary;
    std::string kind;    // "bounty", "mining", "delivery", "supply", "escort", "siege",
                         // "besiege", "contract"
    std::string status;  // how far along: "2 / 5", "ready", "deliver", "waiting at Vela"
    FactionId   faction = Factions::Unaffiliated();  // who gave it
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

    // A faction plan's job (#345): handed in anywhere, about a system that may well be
    // another one, and on the clock.
    bool        job = false;
    std::string where;       // the system's name (a market's contract has one too)
    std::string due;         // "due in 12 min"; empty for no time
    int         siteId = 0;  // a supply job's site, when it is in this system
    // A supply job (#429): delivered by itself at its site or docked in its system, not
    // handed in anywhere. The site by name, wherever it is, and whether the goods are aboard.
    bool        supply = false;
    std::string siteName;
    bool        aboard = false;
    // A siege's job (#407): which side it is for -- the issuer's, besieged or besieging -- and
    // the faction it is against.
    bool      siege = false;
    FactionId against = Factions::Unaffiliated();
    // An escort job's convoy (#430): where it is now (Obs::ConvoyStatus), and whether the pilot
    // has been with it. Its hauler, when it is here, is what the job's hand-in fields point at.
    std::string convoy;
    bool        rode = false;
};

std::vector<Row> Build(const Obs::View& view, Board board);

// "bounty", "mining", "delivery", a plan's "supply", "escort", "siege", "besiege", and a
// market's "contract".
const char* KindWord(int missionType);

}  // namespace MissionList
