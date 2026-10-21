#pragma once

#include "sim/Events.h"
#include "sim/Orders.h"

#include "player/Player.h"
#include "missions/MissionSystem.h"

#include <map>
#include <set>
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

    // The design each owned hull flies (#279 fitting), by hull index: its stock design until
    // it is refitted, then one the server filed (Simulation::Designs). A ship is its design,
    // so this is the ship; a hull with no entry flies its stock design.
    std::map<int, std::string> shipDesigns;
    std::string                DesignOf(int catalogIndex) const;
    // The designs this account saved under names of its own (#279 fitting, step 3): name ->
    // design id, at most Ships::LIBRARY_MAX. The account's names for shared, immutable
    // designs; every id here is kept in the server's store for as long as it is named.
    std::map<std::string, std::string> designLibrary;

    // Insurance (#344): hull index -> the value its policy covers, fixed when it was bought.
    // A hull with no entry is not insured. Spent when the ship is lost.
    std::map<int, double> insurance;
    double                CoverOf(int catalogIndex) const
    {
        const auto it = insurance.find(catalogIndex);
        return it == insurance.end() ? 0.0 : it->second;
    }
    // Where this pilot wakes when their ship is lost (#344): the last station with a shipyard
    // they docked at, or the trade hub of their start; 0 -- none chosen yet. An id is good for
    // one server run; the save names the station (Simulation::SaveAccount).
    int homeStationId = 0;
    // The last ship this pilot lost in this session, for the notice the client shows and the
    // agent reports (#344). `seq` counts losses, so a client can tell a new one from one it
    // has already shown; 0 -- none.
    struct ShipLoss
    {
        int         seq = 0;
        std::string hull;    // the hull's name
        std::string system;  // where it was lost, by name
        double      value = 0.0;
        double      paid = 0.0;  // what insurance paid; 0 -- not insured
        int         cargo = 0;   // units of cargo lost with it
        std::string wokeAt;      // "Station, System", or empty when nowhere would take them
    };
    ShipLoss lastLoss;

    // How this account proves it is itself (#106). The salt is public; the stored value
    // is what a proof is checked against. Both are written to the account file, and the
    // secret they came from is never here.
    std::string authSalt;
    std::string authStored;

    // The account this session plays under (#3). What a player builds is owned by this name
    // (#39), and it outlives the session; empty for a session nobody logged into.
    std::string accountName;

    // Where this player is. Systems other than this one keep running; what makes this one
    // different is only that the NPCs in it can see this ship.
    std::string systemId;

    // Seconds the ship has left in space after the connection dropped (#341); negative while
    // somebody is connected. See Simulation::BeginLinger.
    float lingerLeft = -1.0f;
    bool  Lingering() const { return lingerLeft >= 0.0f; }

    // Verb state the client is not allowed to own, because two copies drift apart on any
    // dropped or duplicated command: the client sends an intent and reads the truth back
    // from the snapshot.
    void ToggleWeapon() { weaponOn = !weaponOn; }
    bool HasRunningOrder() const { return orderStatus == Orders::Status::Running; }
    bool IsDocked() const { return dockedStationId != 0; }

    bool weaponOn = false;
    // What the player has selected, as its last command said (#349): told about in full at
    // every snapshot wherever it is, because a target on the far list would be a target
    // seen once a second.
    int   selectedId = 0;
    float fireTimer = 0.0f;       // weapon cooldown
    float miningProgress = 0.0f;  // accumulator of ore-unit fractions
    int   dockedStationId = 0;    // 0 means in flight
    // Which side of the station the ship is berthed on, as a unit vector from its centre,
    // fixed at the dock (#298). The berth itself moves with the station.
    Vector2 dockBearing = { 1.0f, 0.0f };

    // The hold target as last measured, once per world tick (#298): which object, where it
    // was, and the velocity that came out of the difference. A follow flies that velocity.
    int     holdTrackId = 0;
    Vector2 holdTrackPos = { 0.0f, 0.0f };
    Vector2 holdTrackVel = { 0.0f, 0.0f };

    // The standing order this player has running (#26).
    Orders::Order  order;
    Orders::Status orderStatus = Orders::Status::Idle;
    std::string    orderDetail;             // why it finished or failed
    int            orderId = 0;             // id of the current order
    int            nextOrderId = 0;         // issuance counter
    bool           orderNavIssued = false;  // nav order already given to the ship
    // A route order's plan (#432): the next system, found in `routeIn` at world time
    // `routeAt`. Planned again on arrival in a system and once a second on the way -- the
    // galaxy can change under a journey (a mouth closing, a hole found) -- never every tick:
    // the search reads the pilot's whole index.
    std::string routeNext;
    std::string routeIn;
    double      routeAt = -1.0;
    // What the ship weighs, for the mouths and holes on a route, kept by design id: deriving
    // a design's numbers costs more than the whole search (a design is named by its content,
    // so the id is enough to know it has not changed).
    mutable std::string routeMassDesign;
    mutable double      routeMass = 0.0;
    // Shots a standing order fired (an attack, #39), for the host to add to the beams of the
    // player's system alongside the ones commands fire. Drained every world tick.
    struct Shot
    {
        Vector2 from{ 0.0f, 0.0f };
        Vector2 to{ 0.0f, 0.0f };
        int     targetId = 0;  // what it was fired at (#415)
    };
    std::vector<Shot> orderShots;

    // What this pilot has found, saved with the account (`scanned`): what their scans
    // resolved (#346), by signature key -- a hidden object "<system>/<signature>", a roaming
    // hole "hole:<id>" -- so a hidden base found stays found, and the systems on their own
    // charts (#431 K1), "system:<id>" (Simulation::Learn). `knownRev` changes with it, from
    // the server's one counter: it is part of the key of the pilot's map. How far each scan
    // not yet resolved has got is this session's. A pass under way ends at `scanUntil`
    // (world time; 0 -- none) in `scanSystem`.
    std::set<std::string>        known;
    long long                    knownRev = 0;
    std::map<std::string, float> scanProgress;
    double                       scanUntil = 0.0;
    std::string                  scanSystem;
    // Resolved objects of the system the ship is in that the pilot's layout lacks: the host
    // sends them as a delta of the pilot's own (Simulation::TakeRevealDeltas).
    std::vector<int> revealPending;
    // The holes this pilot knows changed (#342 S9).
    bool routesChanged = false;

    // What happened to this player, in order. Per session rather than per world: one
    // player docking is not news to another, and a shared journal would leak both ways.
    // Capped, and trimmed from the front -- a client further behind than the cap has lost
    // its place anyway and must re-observe.
    void                   RecordEvent(Ev::Kind kind, const std::string& text);
    std::vector<Ev::Event> EventsSince(int seq) const;
    int                    LastEventSeq() const { return nextEventSeq; }

    std::vector<Ev::Event> journal;
    int                    nextEventSeq = 0;
    // How far this pilot has read their corporation's journal (Corps::Corp::journal, #431
    // C2): which corporation, and the last entry they were given. Saved with the account, so
    // what came while they were away is put in this journal at login.
    std::string corpJournalOf;
    int         corpJournalSeen = 0;
};
