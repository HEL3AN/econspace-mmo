#pragma once

#include "sim/Events.h"
#include "sim/Orders.h"
#include "player/Player.h"
#include "missions/MissionSystem.h"

#include <memory>
#include <vector>
#include <string>
#include <vector>

class Ship;

// Everything the server holds on behalf of one connected player (#3).
//
// These fields were members of Simulation, which is why the server could only ever have
// one player: a second connection would have flown the same ship and spent the same
// money. The rules stay in Simulation -- it owns the world and decides what a verb does
// -- and each verb is now told which session it is acting for.
//
// A session is not a connection. It outlives a dropped socket long enough to be saved,
// and an AI commander will eventually hold several of them at once (#32).
struct ClientSession
{
    ClientSession();
    ~ClientSession();
    ClientSession(ClientSession&&) noexcept;
    ClientSession& operator=(ClientSession&&) noexcept;

    int id = 0;  // stable within a server run; 0 is "no session"

    std::unique_ptr<Ship> ship;              // the player's ship, owned by the server
    Player                account{ 500.0 };  // money, skills, reputation, wanted levels
    MissionSystem         missions;          // the board at the docked station, and the log

    // Which ships this account owns, as catalog indices, and which one it is flying
    // (#5). The server holds them because it is the server that charges for a ship: a
    // client that owned this list could switch to anything in the catalog for nothing,
    // which is exactly what it could do before.
    std::vector<int> ownedShips;
    int              currentShip = 0;

    bool Owns(int catalogIndex) const
    {
        for (int i : ownedShips)
            if (i == catalogIndex)
                return true;
        return false;
    }

    // Where this player is. Systems other than this one keep running; what makes this one
    // different is only that the NPCs in it can see this ship.
    std::string systemId;

    // Verb state the client is not allowed to own, because two copies drift apart on any
    // dropped or duplicated command: the client sends an intent and reads the truth back
    // from the snapshot.
    void ToggleWeapon() { weaponOn = !weaponOn; }
    bool HasRunningOrder() const { return orderStatus == Orders::Status::Running; }
    bool IsDocked() const { return dockedStationId != 0; }

    bool  weaponOn = false;
    float fireTimer = 0.0f;       // weapon cooldown
    float miningProgress = 0.0f;  // accumulator of ore-unit fractions
    int   dockedStationId = 0;    // 0 means in flight

    // The standing order this player has running (#26).
    Orders::Order  order;
    Orders::Status orderStatus = Orders::Status::Idle;
    std::string    orderDetail;             // why it finished or failed
    int            orderId = 0;             // id of the current order
    int            nextOrderId = 0;         // issuance counter
    bool           orderNavIssued = false;  // nav order already given to the ship

    // What happened to this player, in order. Per session rather than per world: one
    // player docking is not news to another, and a shared journal would leak both ways.
    // Capped, and trimmed from the front -- a client further behind than the cap has lost
    // its place anyway and must re-observe.
    void                   RecordEvent(Ev::Kind kind, const std::string& text);
    std::vector<Ev::Event> EventsSince(int seq) const;
    int                    LastEventSeq() const { return nextEventSeq; }

    std::vector<Ev::Event> journal;
    int                    nextEventSeq = 0;
};
