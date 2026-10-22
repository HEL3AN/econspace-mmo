#pragma once

#include "raylib.h"
#include "core/Faction.h"
#include "core/ShipDesign.h"
#include "core/WorldLoader.h"  // Universe, the galaxy index
#include <functional>
#include <map>
#include <memory>
#include <set>
#include "entities/Defence.h"
#include "entities/EntityKind.h"
#include "sim/Events.h"
#include "sim/Orders.h"
#include "sim/SystemState.h"  // FireEvent

#include <string>
#include <vector>

// Client<->server protocol (track M, M4). The CLIENT sends `Command` (player
// intents), the SERVER returns `Snapshot` (view of the player's system). JSON
// serialization (readable, debuggable; can switch to binary later). Types are
// render-independent: role/kind stored as numbers, no protocol dependency on game classes.
namespace Proto
{

// The most systems a route order may name to pass through on its way (#432).
inline constexpr size_t ROUTE_WAYPOINTS_MAX = 32;

// Wire format version. Bump it whenever a message changes in a way an older build would
// misread — a renamed or repurposed field, a changed unit, a changed meaning.
//
// Every message carries it and every Decode* rejects anything else. That check is the
// whole point: per-field decoding is deliberately permissive (`value(key, default)`), so
// without it a client built against an older protocol would silently receive defaults
// instead of an error, and the failure would surface much later as a ship that does not
// move or an account that reads zero.
inline constexpr int PROTO_VERSION =
    52;  // 52: a galaxy stat says whether the recipient's corporation's front is open there
         // (#431 slice 8);
         // 51: PvP under the law (#431 slice 10): another pilot's ship says whether they are
         // flagged a criminal and their security status, and the player their own status and
         // what is left of their flag;
         // 50: a structure's layout carries when its damage was set and whether it mends, and
         // its damage mends on the clock at its blueprint tier's rate (#431 slice 7);
         // 49: influence and claims (#431 slice 5) -- a galaxy stat names the corporation that
         // holds the system and the fee its markets take, a CorpAct carries a policy's value
         // (tax, docking), and a CorpView the treasury, the policies and the systems held;
         // 48: the index and the galaxy map are each pilot's own (#431): what they and their
         // corporation have charted, beside the public charts, and a system says whose
         // charts it is on (`known`) and which faction charted it (`chartedBy`);
         // 47: a route order carries its waypoints and a preference -- shortest, safer or less
         // secure -- in place of avoid-danger, and the index marks an unstable mouth's link
         // with when it closes and the mass it lets by (#432);
         // 46: an escort job carries its convoy -- where it is, where it goes next, its hauler and
         // escorts when they are in the recipient's system, and whether the pilot has been with
         // it (#430);
         // 45: a hello carries the client's build and a hash of each shared data file, and the
         // server refuses a mismatch (#400);
         // 44: a siege's job for the besieging side (#407) -- a mission of a new type, and
         // a siege's job of either side names the faction it is against; 43: corporations (#347) --
         // a CorpAct from a client, a CorpView to each member and invitee, a corp channel in
         // ChatSay and ChatLine, each local pilot's ticker in ChatMembers, and a director reads a
         // member's allow-list; 42: a mission may be a short market's contract (#343, #407), with
         // its system and deadline; 40 and 41 were reserved for corporations and siege jobs, which
         // landed as 43 and 44, and never shipped; 39: roaming holes and the scanner (#342 S9,
         // #346) -- a scan in a command, the scanner and the system's signatures in the player's
         // snapshot, a hidden object in a pilot's layout only once their scan resolved it, and a
         // link of the index may be a hole with its tail; 38: NPC haulers (#343)
         // -- a ship's cargo in the snapshot, a wreck's in its layout; what is left of each belt
         // (#348) in the snapshot; 37: chat -- a ChatSay from a client, ChatLines and ChatMembers
         // from the server (#347); 36: reserved for the haulers and never shipped; 35: a lost ship
         // is lost -- insurance in a command, each hull's cover and the last loss in the player's
         // snapshot, and death wakes the pilot docked at home (#344); 34: a mission may be a
         // faction plan's job (#345), with its plan, system, deadline and site; 33: goods from
         // data/goods.json (#343) -- cargo and the market by its order, the market a bid, an ask
         // and a stock per good, a buy in a command and in its acknowledgement; 32: the named
         // design library -- save, apply, rename and delete in a command, the library in the
         // player's snapshot (#279 fitting, step 3); 31: the server's faction registry, sent before
         // anything names one of its factions
         // (#342 S5, #355); 30: a snapshot is numbered and may be a delta against one the client
         // acknowledged with an Ack; entities are quantised integers, and beyond the area of
         // interest they are coarse and refreshed once a second; missions are part of the delta
         // (#349);
         // 29: a gate's expiresAt is when an unstable wormhole mouth collapses (#342 S4); 28: a
         // structure's allow-list is in its owner's layout only; anyone else's has its mode and
         // threshold with the lists empty (#340); 27: a structure's rules of engagement, set by a
         // command and carried in its layout; whether a system is claimed, in the galaxy stats
         // (#340); 26: a refit, the whole fit asked for in a command (#279 fitting); 25: a login
         // may be answered with the starts to choose from, and a ship exists only once one is
         // chosen
         // (#342 S2); 24: a ship flies a design of its own, filed by a hash of its content; a
         // DesignDef tells a client what one is before any snapshot names it (#279 fitting); 23: a
         // faction is named by its string id, never by index (#355); 22: a snapshot without
         // missions means they are unchanged, and a command leaves out every field at its default
         // (#341); 21: the build order, with its blueprint and name (#39); 20: reserved for
         // #279's design ids, which landed as 24; 19: player hulls are designs -- a hull index
         // means derived stats, and another player's ship carries its design (#279 step 5); 18:
         // an NPC's ship design (#279); 17: dismantling, a structure's blueprint and damage, the
         // attack order (#39); 16: follow (nav 5, hold 3) and hold orders (#298); 15: structures
         // and deploying them (#39); 14: layout revisions and LayoutDelta (#38)

// --- Command: client -> server, every tick ---
// The first thing a client says, before any command: who it is (#3).
//
// Until it arrives the server has a socket and no player. It cannot invent one: with
// several clients connected there is no such thing as "the account", and the progress a
// returning player expects is stored under a name. A name is not proof of identity --
// authentication is a separate problem, and this is the hook it will hang on.
struct Hello
{
    std::string account;  // [A-Za-z0-9_-], 1..32 characters; the server rejects the rest
    // Which build this is and what it read (#400): Version::NUMBER, and Compat::DataHashes of
    // the client's data/. The server lets in only its own build reading the same data.
    std::string                        build;
    std::map<std::string, std::string> data;
};

// The server ending a session on purpose, and saying why (#105).
//
// Until this existed the only way to refuse anyone was to drop the socket, which the
// client cannot tell apart from a crash, a cable, or a server that fell over. A player
// displaced from their own account deserves to be told that is what happened rather than
// left guessing -- and refusing a login for any other reason (#106) needs the same
// channel.
// The server's half of the handshake (#106): a nonce for this connection, and the salt
// the account's secret is stored under. `isNew` says there is no such account yet, in
// which case the salt is a fresh one and this login creates it.
struct Challenge
{
    std::string nonce;
    std::string salt;
    bool        isNew = false;
};

// The client's answer: proof that it knows the secret, good for this nonce only.
//
// `stored` is sent on registration and never again. It is the one moment the value a
// server keeps crosses the wire, which is a real weakness of doing this without transport
// encryption -- and the alternative, sending the secret itself on every login, is worse.
struct Auth
{
    std::string proof;
    std::string stored;
};

struct Bye
{
    std::string reason;  // shown to the player as-is; short, and in plain words
};

// Where a pilot may begin (#342 S2). Sent after a login when the account has no place in
// this galaxy -- a new account, or one whose system is gone -- and before anything else:
// until the client answers with ChooseStart the player has no ship, and the server sends
// no index, layout or snapshot.
struct StartOption
{
    std::string system;   // the system id to answer with
    std::string name;     // what it is called
    std::string station;  // where you arrive: the heart's trade hub
    std::string owner;    // whose space it is, a faction id
};
struct Starts
{
    std::vector<StartOption> options;
    // The server deals starts itself: the client may only ask for one at random.
    bool randomOnly = false;
};
// The answer: a system from the list, or empty for "any of them" -- the server's pick.
struct ChooseStart
{
    std::string system;
};

struct Command
{
    int seq = 0;  // input number (for client prediction/reconciliation, M4e)
    // Held control axes.
    bool  thrust = false;
    float turn = 0.0f;
    bool  brake = false;
    // One-shot intents (edge).
    bool toggleStabilizer = false;
    bool toggleMining = false;
    bool toggleWeapon = false;
    bool dock = false;
    bool undock = false;
    // Combat target / navigation / jump.
    int targetId = 0;  // selected agent (0 — none)
    // Navigation order. Modes 1 and 2 are one-shot -- fly there, or warp there, and stop.
    // Modes 3 to 5 are standing: hold station on an object until released, which is what
    // flying a fight or waiting at a gate actually is (#157).
    //   0 none, 1 autopilot, 2 warp, 3 orbit, 4 keep at range, 5 follow (keep the range
    //   and match the target's velocity, #298)
    int     navMode = 0;
    Vector2 navTarget = { 0.0f, 0.0f };
    float   navStopDist = 0.0f;
    // A warp's bend point, if it has one: around a body rather than through it (#160).
    // Chosen by whoever orders the warp, so both sides fly the same two legs.
    bool    navViaSet = false;
    Vector2 navVia = { 0.0f, 0.0f };
    // For the standing modes: what to hold station on, and at what distance. An id rather
    // than a point, because the point of it is to follow something that moves.
    int   navHoldId = 0;
    float navRange = 0.0f;
    int   jumpGateId = 0;  // jump through gate by id (0 — none)
    int   lootId = 0;      // loot derelict by id (0 — none)
    // Station trade (server-authoritative, M4e-3b): sell resource / refit.
    int sellType = -1;   // ResourceType as int (-1 — no sale)
    int sellAmount = 0;  // amount to sell (server clamps to cargo)
    // A purchase (#343): as much of the good as the stock, the hold and the money allow, up
    // to the amount.
    int buyType = -1;  // ResourceType as int (-1 — no purchase)
    int buyAmount = 0;
    int refitShip = -1;  // switch to an ALREADY bought ship (index; -1 none)
    // Refit the ship being flown to this fit, at the shipyard it is docked at (#279
    // fitting). The fit is the whole fit, not a change to it; the hull stays the hull. The
    // server checks it, prices it and charges for it; a refusal is a Notice saying why.
    bool                        refit = false;
    std::vector<Ships::FitLine> refitLines;
    // The named design library (#279 fitting, step 3), each by the name the player gave it;
    // empty -- not this command. Save files `saveLines` on the hull being flown under
    // `saveDesign`; apply refits to a saved design as `refit` would; rename gives
    // `renameDesign` the name `renameTo`. A refusal is a Notice saying why.
    std::string                 saveDesign;
    std::vector<Ships::FitLine> saveLines;
    std::string                 applyDesign;
    std::string                 renameDesign;
    std::string                 renameTo;
    std::string                 deleteDesign;
    // Account actions (server-authoritative, M4f): buy ship / pay bounty / cheat.
    int         buyShip = -1;          // buy ship by catalog index (-1 none)
    int         insureShip = -1;       // insure an owned hull by catalog index (-1 none, #344)
    std::string payBountyFaction;      // pay off bounty with a faction (its id; empty none)
    bool        debugMoney = false;    // F1: grant debug money
    int         acceptOffer = -1;      // accept mission offer by board index (-1 none)
    int         completeMission = -1;  // hand in active mission by index (-1 none)
    // Name the system the ship is in, as its discoverer (#145); empty -- not this command.
    std::string nameSystem;

    // Standing order (one-shot, #26/#72). The client sends an INTENT, exactly as it does
    // for docking or trading: the server decides whether the order is possible and owns
    // everything about running it. An agent can do nothing a human client could not.
    int         orderKind = 0;  // Orders::Kind as int; 0 — no order this command
    int         orderTarget = 0;
    Vector2     orderPoint = { 0.0f, 0.0f };
    float       orderStopDist = 120.0f;
    bool        orderWarp = false;
    bool        orderUntilFull = false;
    std::string orderDestSystem;  // Route
    // Route: systems to pass through first, in order, and how to weigh a hop
    // (Route::Preference: 0 shortest, 1 safer, 2 less secure) (#432). Until #171 nothing on
    // the wire could ask for a safer way, so `travel_to_system`'s avoid_danger was accepted,
    // advertised to models in a prompt, and silently dropped.
    std::vector<std::string> orderWaypoints;  // at most ROUTE_WAYPOINTS_MAX
    int                      orderPreference = 0;
    // Build: the blueprint, and what to call the site (#39). The point is `orderPoint`.
    std::string orderBlueprint;
    std::string orderName;
    bool        abortOrder = false;  // drop whatever is running

    // Lay down a construction site (#39): the blueprint's id, where, and what to call it
    // (empty -- the blueprint's name). Empty `deploy` -- not this command. The server checks
    // everything: the hold, the place, the caps; a refusal is a Notice saying which.
    std::string deploy;
    Vector2     deployPos = { 0.0f, 0.0f };
    std::string deployName;
    // Take one's own site or structure apart, by id (#39); 0 -- not this command. What comes
    // back goes into the hold; a refusal is a Notice saying why.
    int dismantleId = 0;
    // Set the rules of engagement of one's own armed structure, by id (#340); 0 -- not this
    // command. A refusal is a Notice saying why, and the change reaches everyone as a delta.
    int     defenceId = 0;
    Defence defence;
    // Start a pass of the ship's scanner (#346); the signatures it resolves are this pilot's.
    bool scan = false;
};

// Entity kind. The wire uses the engine's enum rather than a parallel copy, so there is
// no translation between "what the world says an object is" and "what the snapshot says",
// and no chance of the two drifting apart.
using EntityKind = ::EntityKind;

// --- Snapshot: server -> client ---
struct EntitySnapshot
{
    int         id = 0;
    EntityKind  kind = EntityKind::Unknown;
    Vector2     pos = { 0.0f, 0.0f };
    float       heading = 0.0f;
    float       size = 0.0f;
    FactionId   faction = Factions::Unaffiliated();
    int         role = -1;        // NpcRole as int (-1 — not an NPC)
    float       hullFrac = 1.0f;  // hull fraction 0..1 (for combatants)
    int         ore = -1;         // ResourceType as int for fields (-1 — not a field)
    std::string name;
    // The ship design an NPC or another player flies (#279), by id in data/ships.json; empty
    // for anything else. It is what the client draws, so it is the server's word, not a guess
    // from the role.
    std::string design;
    // What it carries (#343): an NPC hauler's cargo, a wreck's (filled in from the layout by
    // CompleteFromLayout); -1 -- nothing.
    int cargo = -1;  // ResourceType as int
    int cargoAmount = 0;
    // A belt's ore (#348): what is left and what it holds full, filled in from the
    // snapshot's `belts` by CompleteFromLayout; -1 -- not a belt, or not told.
    int oreLeft = -1, oreMax = -1;
    // Another pilot under the law (#431 slice 10): flagged a criminal -- anyone may shoot them
    // without a crime -- and their security status, -10..+10.
    bool  criminal = false;
    float securityStatus = 0.0f;
};

// One entry of a player's design library: their name for it, and the design's id.
struct SavedDesign
{
    std::string name;
    std::string design;
};

// The last ship a player lost (#344), as the client's notice and an agent's observe tell
// it. `seq` counts the losses of this session: a new one is one with a higher seq.
struct LossView
{
    int         seq = 0;  // 0 -- none
    std::string hull;     // the hull's name
    std::string system;   // where, by name
    double      value = 0.0;
    double      paid = 0.0;  // what insurance paid
    int         cargo = 0;   // units of cargo lost
    std::string wokeAt;      // "Station, System"; empty -- nowhere took them
};

// One signature in the system, as this pilot's scanner shows it (#346). Until it is resolved
// only its code and how much of it is resolved are known; then what it is, and the object,
// which the pilot's layout now has.
struct SignatureView
{
    std::string code;             // "KQX-417": the same for everyone
    float       progress = 0.0f;  // 0..1; 1 -- resolved
    std::string kind;             // resolved: "wormhole", "hidden base"; empty until then
    int         id = 0;           // resolved: the object's id in the layout
    std::string dest;             // a wormhole: the system it leads to
};
// The ship's scanner (#346): a pass under way until `until` (world time; 0 -- none), and the
// signatures of the system the ship is in, by code.
struct ScanView
{
    double                     until = 0.0;
    double                     seconds = 0.0;  // what a pass takes
    std::vector<SignatureView> sigs;
};

// Player ship state in the snapshot.
struct PlayerView
{
    // The hangar, as the server sees it (#5). The client used to keep this itself, which
    // meant it survived nothing and could be disagreed with.
    std::vector<int> ownedShips;
    int              shipIndex = 0;
    // The design each of those hulls flies, in the same order (#279 fitting): the hull's
    // stock design until it is refitted, then one the server filed. Every id here that is
    // not a stock one has had its DesignDef sent first.
    std::vector<std::string> shipDesigns;
    // The account's saved designs, by name (#279 fitting, step 3); each id that is not a
    // stock one has had its DesignDef sent first, so the client can price and derive it.
    std::vector<SavedDesign> library;
    // What each of those hulls is insured for, in the same order; 0 -- not insured (#344).
    std::vector<double> insured;
    LossView            loss;
    ScanView            scan;  // (#346)

    Vector2 pos = { 0.0f, 0.0f };
    Vector2 vel = { 0.0f, 0.0f };
    float   heading = 0.0f;
    float   hull = 0.0f, maxHull = 0.0f, shields = 0.0f, maxShields = 0.0f;
    int     cargoUsed = 0, cargoCap = 0;
    int     dockedStationId = 0;  // docked station id (0 — in flight); server-authoritative
    int     warpPhase = 0;        // 0 none, 1 aligning, 2 warp
    // Server-authoritative navigation (warp/autopilot): the client mirrors it and keeps
    // no warp-timer scale of its own (otherwise "bar ready" and flight start diverge).
    float   warpAlign = 0.0f;  // remaining warp spin-up timer
    Vector2 warpTarget = { 0.0f, 0.0f };
    float   warpDrop = 0.0f;     // warp exit distance
    bool    warpViaSet = false;  // still on the first leg of a bent warp (#160)
    Vector2 warpVia = { 0.0f, 0.0f };
    bool    autopilot = false;
    Vector2 apTarget = { 0.0f, 0.0f };
    float   apStop = 0.0f;  // autopilot stop distance
    // The standing hold, if any (#157). Mirrored like the warp and the autopilot: the
    // client has to have it in order to replay unacknowledged inputs on top of a snapshot,
    // and it is what the HUD reports the ship is doing.
    int   holdMode = 0;  // 0 none, 1 orbit, 2 keep at range, 3 follow (#298)
    int   holdTargetId = 0;
    float holdRange = 0.0f;
    bool  stabilizer = true;
    bool  mining = false;
    bool  weaponOn = false;
    bool  docked = false;
    int   nearbyStationId = 0;
    int   lastInput = 0;  // last input number the server processed (ack, M4e)
    // The running standing order, so a client can see what it asked for is under way.
    // The journal (#29) reports outcomes; this reports what is in progress.
    int              orderKind = 0;    // Orders::Kind as int
    int              orderStatus = 0;  // Orders::Status as int
    int              orderId = 0;
    std::string      orderDetail;  // why it finished or failed
    std::vector<int> cargoByType;  // remaining cargo per resource (AllResourceTypes order)
    // Player account (server-authoritative, M4f): the client shows it as a mirror.
    // reputation/bounty — per faction (index = FactionId in this process's registry; on the
    // wire, keyed by faction id), skillXp — Piloting/Mining/Trading.
    double              money = 0.0;
    std::vector<float>  reputation;  // one per faction
    std::vector<double> bounty;      // one per faction
    std::vector<float>  skillXp;     // 3 skills (Piloting/Mining/Trading)
    // Under the law (#431 slice 10): the pilot's security status, -10..+10, and the seconds
    // left on their criminal flag (0 -- not flagged).
    float  securityStatus = 0.0f;
    double criminalLeft = 0.0;
};

// Trade acknowledgment (server -> client): what a sale or a purchase came to, for the client
// to say. The account itself is the server's, mirrored in the snapshot.
struct TradeAck
{
    int    type = 0;       // ResourceType as int
    int    sold = 0;       // actually sold
    double gross = 0.0;    // gross revenue at the server price
    double revenue = 0.0;  // net revenue the server credited to the account (for display)
    int    bought = 0;     // actually bought (#343)
    double cost = 0.0;     // what the purchase took from the account, the fee included
    double fee = 0.0;      // the fee in either (#348)
};

// Mission view in the snapshot (server-authoritative, M4f-2). Render-independent copy
// of Mission: stations addressed by stable id, completable computed by the server.
struct MissionView
{
    int         type = 0;  // MissionType as int
    FactionId   faction;   // the issuer
    std::string title;
    std::string description;
    int         giverStationId = 0;
    int         destStationId = 0;
    int         resource = 0;  // ResourceType as int
    int         targetCount = 0;
    int         progress = 0;
    double      rewardMoney = 0.0;
    float       rewardRep = 0.0f;
    bool        completable = false;
    // A plan's job (#345): the plan, the system it is about -- by id and by name, since the
    // client sees only its own -- by when it must be done (world time; 0 for no time), and a
    // supply job's site when it is in the recipient's system (0 otherwise).
    int         planId = 0;
    std::string systemId;
    std::string systemName;
    double      deadline = 0.0;
    int         siteId = 0;
    std::string siteName;  // ...and a supply job's site by name, wherever it is (#429)
    // A siege's job, either side's (#407): the faction it is against. The issuer is the side
    // it is for.
    FactionId against = Factions::Unaffiliated();
    // An escort job's convoy (#430), as it is now rather than as the description had it when
    // the job was posted: the system it is in -- crossing it, or waiting there to set out --
    // by id and by name; the next system on its way (empty in the last one, where its site
    // is); its hauler and the warships with it, by entity id, only while they are in the
    // recipient's system (0 and none otherwise); and whether the pilot has been with it.
    std::string      convoySystemId;
    std::string      convoySystemName;
    std::string      convoyNextName;
    bool             convoyMoving = false;  // on its way; false -- waiting to set out
    int              haulerId = 0;
    std::vector<int> convoyEscortIds;
    bool             rode = false;
};

// Snapshot of the player's system — what the client draws.
struct Snapshot
{
    std::string systemId;
    // The world's clock, in seconds (#192). Everything that moves is a function of time
    // (#136), and with each client's own clock two players saw one station turned two
    // ways; with this they share the phase as well as the rate.
    double                      time = 0.0;
    PlayerView                  player;
    std::vector<EntitySnapshot> entities;
    std::vector<FireEvent>      fires;
    std::vector<Ev::Event>      events;  // journal entries the client has not seen (#29)
    // The system's market, per good in AllResourceTypes order (#343): what it pays for a unit
    // now, what it asks for one, and how many it holds.
    std::vector<float> marketPrices;
    std::vector<float> marketAsk;
    std::vector<int>   marketStock;
    // What is left of each belt in the system (#348), by entity id: the ore now and full. It
    // changes as the belts are mined and grow back, which the layout does not follow.
    struct BeltOre
    {
        int id = 0, ore = 0, max = 0;
    };
    std::vector<BeltOre>     belts;
    std::vector<TradeAck>    tradeAcks;      // one-shot trade acknowledgments (M4e-3b)
    std::vector<MissionView> missionOffers;  // docked station board (empty outside dock)
    std::vector<MissionView> missionActive;  // player's active missions (M4f-2)
};

// Static description of a system entity, for building the client proxy WITHOUT
// access to the live server world (layout, M4d-3c). Sent once on entering a system;
// dynamics (position/heading/hull) arrive in Snapshot by id. Carries what the engine
// entity's constructor/Draw needs. NPCs are NOT included here (they are snapshot dynamics).
struct EntityLayout
{
    int         id = 0;
    EntityKind  kind = EntityKind::Unknown;
    Vector2     pos = { 0.0f, 0.0f };
    float       size = 0.0f;
    Color       color = { 255, 255, 255, 255 };
    std::string name;
    FactionId   faction = Factions::Unaffiliated();
    int         subType = 0;         // StarType/PlanetType/StationRole as int
    float       orbitRadius = 0.0f;  // planet: orbit radius (for the ring)
    int         resource = -1;       // ResourceType: planet deposit / field ore (-1 none)
    double      reward = 0.0;        // derelict: loot reward
    std::string dest;                // gate: destination system id
    // The archetype id (#195). What an object can do is its components (#34), and a client
    // or an agent that knows the archetype can ask the registry instead of guessing from
    // the kind -- which a dock a player builds (#44) would not match.
    std::string archetype;
    // Who put it there (#38): an account name, or empty for the world itself -- everything
    // the region and the data files make. Carried so a client can tell a player's structure
    // from the world's without asking; what an owner may do with it is the server's call.
    std::string owner;
    bool        looted = false;  // derelict: already searched -- a wreck changes once (#38)
    // derelict: what a hauler's wreck holds (#343), ResourceType as int (-1 nothing), and how
    // many; a search that takes part of it is a change, and comes as a delta.
    int cargo = -1;
    int cargoAmount = 0;
    // A structure's time line, in world seconds (#39). While `completesAt` is ahead of the
    // clock it is a site and `archetype` is what it will become; 0 -- built. `expiresAt` 0 --
    // it stays. Instants rather than a progress figure, so the site advances on every client
    // without another word from the server. For a gate, `expiresAt` is when an unstable
    // wormhole mouth collapses unless it is anchored (#342 S4); 0 -- it stays.
    double startedAt = 0.0;
    double completesAt = 0.0;
    double expiresAt = 0.0;
    // A structure's blueprint and the damage it has taken (#39). Its hull at any instant is
    // Structure::MaxHull of the time line, so a site's grows on every client with no word
    // from the server; a hit is a change and comes as a delta.
    // The damage is as of `damagedAt`, and it mends from then at the blueprint tier's rate
    // (#431 D1) unless `mends` is false -- a faction's outpost nobody supplies -- so a client
    // shows it mending with no word from the server, as it shows a site going up.
    std::string blueprint;
    float       damage = 0.0f;
    double      damagedAt = 0.0;
    bool        mends = true;
    // A player's armed structure's rules of engagement (#340): what it fires at, and so what
    // it costs its owner. Default -- return fire.
    Defence defence;
};

// Full static "layout" of a system — what the client builds the world proxy from.
struct SystemLayout
{
    std::string systemId;
    // How many times this system's static layer has changed since the server started
    // (#38). A delta carries the revision it brings the system to, so a client that was
    // sent a layout already containing a change can tell the delta about it is old news.
    int                       rev = 0;
    std::vector<EntityLayout> entities;
};

// What changed in a system's static layer since the last delta (#38): sent to everyone
// standing in that system, in between the snapshots, instead of the whole layout again.
//
// `added` and `changed` are both the object's whole current description, so applying one
// is a replace rather than a patch, and applying it twice is the same as once. They are
// kept apart because they mean different things to a player -- a structure appearing is
// news, a wreck being searched is not.
//
// The ordering guarantee lives on the server: a delta is sent before any snapshot built
// after the change, on the same ordered connection, so a client never sees a snapshot
// that refers to an object its layout does not have -- or misses that one is gone.
struct LayoutDelta
{
    std::string               systemId;
    int                       rev = 0;  // the revision this delta brings the system to
    std::vector<EntityLayout> added;
    std::vector<EntityLayout> changed;
    std::vector<int>          removed;
};

// What of the static layer only its owner may read (#340): an armed structure's allow-list,
// which says whom its owner trusts. Everyone in a system is sent the same layout and the
// same deltas, so the server cuts them for each viewer before they go: an entity `viewer`
// does not own keeps its mode and threshold, and its lists are emptied. An empty `viewer`
// owns nothing. True when anything was cut.
bool ForViewer(std::vector<EntityLayout>& v, const std::string& viewer);
bool ForViewer(SystemLayout& lay, const std::string& viewer);
bool ForViewer(LayoutDelta& d, const std::string& viewer);
// ...and the same for a viewer who may read more than their own (#347): `reads(owner)` says
// whether they may read what `owner` keeps private -- their own, and as the CEO or a director
// of a corporation, its members'.
using ReadsOwner = std::function<bool(const std::string& owner)>;
bool ForViewer(std::vector<EntityLayout>& v, const ReadsOwner& reads);
bool ForViewer(SystemLayout& lay, const ReadsOwner& reads);
bool ForViewer(LayoutDelta& d, const ReadsOwner& reads);
// Whether the delta carries anything only `account` may read, so theirs is not everyone's.
bool OwnsPrivate(const LayoutDelta& d, const std::string& account);
bool OwnsPrivate(const LayoutDelta& d, const ReadsOwner& reads);

// A client's copy of the static layer of the system it is in, kept current by layouts and
// deltas. Both clients -- the game and econagent -- keep one, so the rule for applying a
// delta lives here once rather than twice.
struct LayoutMirror
{
    std::string                 systemId;
    int                         rev = 0;
    std::map<int, EntityLayout> byId;

    // A whole layout replaces everything: it is sent on entering a system.
    void Reset(const SystemLayout& lay);
    // Applies a delta for this system that is newer than what is held. False -- and
    // nothing touched -- for a delta about another system (one sent just before a jump)
    // or one the layout already included (a layout built after the change, sent before
    // the delta about it).
    bool Apply(const LayoutDelta& d);
};

// Per-system statistics for the galaxy map (M4e-3c): dynamics the server computes
// (macro) — the client does not simulate them itself, so it receives them by snapshot.
struct GalaxySystemStat
{
    std::string id;
    float       security = 0.5f;
    int         pirates = 0;
    float       prosperity = 0.5f;
    FactionId   controller = Factions::Unaffiliated();
    // Whether the controller holds it, so its law applies (#340): what a structure firing
    // first there costs its owner is read from this and the controller.
    bool claimed = false;
    // The player corporation that holds it (#431 slice 5): its id, ticker and name, empty for
    // none -- a client knows only its own corporation, so the rest travels with the id.
    // (Braced: an aggregate initialised without them is not a missing initializer.)
    std::string holder{};
    std::string holderTicker{};
    std::string holderName{};
    // The share of a trade its markets take (T3): the holder's tax. Below zero: the standard
    // fee (Goods::Rules().fee) -- which is what it is wherever no corporation holds.
    float fee = -1.0f;
    float FeeOr(float standard) const { return fee < 0.0f ? standard : fee; }
    // The front line (#431 D3): another side holds it, and the recipient's corporation may
    // besiege or lay a claim there -- it holds a system next door, or one next door is nobody's.
    // False for a pilot in no corporation, and wherever nobody else holds.
    bool front = false;
};

// Galaxy snapshot: statistics for the systems on the recipient's charts + a news feed (#431:
// each pilot's own). Sent by the server periodically (about once a second) — the networked
// galaxy map comes alive.
struct GalaxyState
{
    std::vector<GalaxySystemStat> systems;
    std::vector<std::string>      events;
};

// What a design a player made is (#279 fitting), sent once per connection the first time a
// client meets its id, and before any snapshot that names it -- the rule LayoutDelta keeps
// (#38). A stock design is never sent: both sides read it from ships.json. The id is a hash
// of the design (Ships::ContentId), and a decoder checks it, so a definition can never
// rename another one.
struct DesignDef
{
    Ships::Design design;  // design.id is the id
};

// The server's faction registry (#355, #342 S5): every faction it has, and every pair whose
// stance is not neutral. Sent at login before the index, and again whenever the registry
// grows -- a region opening with natives of its own -- so a client and an agent know a
// faction the server made by its name rather than reading it as unaffiliated. A client adds
// what it lacks to its own registry (Factions::Add) and takes the server's stances.
struct FactionRelation
{
    std::string a, b;  // faction ids
    Stance      stance = Stance::Neutral;
};
struct FactionList
{
    std::vector<FactionDef>      factions;  // id, name, colour, lawful, kind; no temperament
    std::vector<FactionRelation> relations;
};
// This process's registry as the message.
FactionList RegistryMessage();
// ...and a message into this process's registry: the factions it lacks added, every stance
// the message names set.
void ApplyFactions(const FactionList& list);

// --- Chat (#347) ---------------------------------------------------------------------------
// Pilots talk: to the system they are in, to one pilot by name, and the server's operator to
// everyone. Talking is not a tick of anything, so it is not a Command -- one would carry the
// text sixty times a second, or spend a tick budget a sentence has no business spending.

// What a pilot says (client -> server). `to` empty: the local channel, everyone in the
// system; otherwise the pilot it is for, by account name. The server cleans the text up
// (Chat::Sanitize) and decides who hears it.
struct ChatSay
{
    std::string to;
    std::string text;
    // "corp": to every online member of the sender's corporation (#347), `to` ignored;
    // empty: as `to` says.
    std::string channel;
};

// One line as a client shows it (server -> client).
struct ChatLine
{
    // "local" -- said in a system to everyone there; "private" -- from one pilot to another,
    // sent to both; "server" -- the operator, to everyone; "notice" -- the server, to this
    // pilot alone: why a line was not sent.
    std::string channel;
    std::string from;    // who said it; empty for the server
    std::string to;      // a private line's recipient
    std::string system;  // where a local line was said
    std::string corp;    // a corp line's corporation, by ticker (#347)
    std::string text;
    long long   at = 0;  // when, in Unix seconds
    // Said before this pilot arrived: the short history a system gives whoever comes in.
    bool history = false;
};
struct ChatLines
{
    std::vector<ChatLine> lines;
};

// Who is in the local channel (server -> client): sent on entering a system and whenever
// someone comes or goes. Account names, sorted.
struct ChatMembers
{
    std::string              system;
    std::vector<std::string> pilots;
    // Each pilot's corporation ticker, in the same order; empty for one in none (#347).
    std::vector<std::string> corps;
};

// --- Corporations (#347) -------------------------------------------------------------------
// A corporation is its own registry on the server (Corps::Registry), not a faction: it is
// named by a string id, and by its ticker where a player reads it.

// A verb (client -> server): "create" (name, ticker), "invite" (pilot), "join" and "decline"
// (ticker), "leave", "kick" (pilot), "role" (pilot, role: "ceo", "director", "member"); a
// holder's policies (#431 slice 5): "tax" (value: a percentage, or "standard") and "docking"
// (value: "own", "allies", "all"). Not a tick of anything, like a chat line. The answer is a
// chat notice to the sender, and a CorpView to everyone whose picture of it changed.
struct CorpAct
{
    std::string verb;
    std::string name;
    std::string ticker;
    std::string pilot;
    std::string role;
    std::string value{};  // braced: the verbs without one leave it out
};

struct CorpMember
{
    std::string pilot;
    std::string role;  // "ceo", "director", "member"
    bool        online = false;
};
struct CorpInvite
{
    std::string name;
    std::string ticker;
    std::string by;
    long long   at = 0;  // Unix seconds
};
// One pilot's corporation as they see it (server -> client): sent at login and whenever it
// changes -- someone joins, leaves, comes online, is given a role -- and the invites waiting
// for them. `id` empty: in none.
struct CorpView
{
    std::string             id;
    std::string             name;
    std::string             ticker;
    long long               founded = 0;
    std::vector<CorpMember> members;
    std::vector<CorpInvite> invites;  // waiting for this pilot's answer
    // Its money and what it holds (#431 slice 5): the treasury, the tax at its markets as a
    // share (the standard one when it set none), its docking policy ("own", "allies", "all"),
    // and the systems it holds, by id.
    double                   treasury = 0.0;
    float                    tax = 0.0f;
    std::string              docking = "all";
    std::vector<std::string> holds;

    // This pilot's role in it; empty when in none.
    std::string RoleOf(const std::string& pilot) const;
    bool        Has(const std::string& pilot) const { return !RoleOf(pilot).empty(); }
};

// JSON serialization. Decode* return false on a broken/unsuitable message.
// Fills in what the per-tick snapshot deliberately leaves out (#16, #97). A star does not
// move, change name or change owner, so a snapshot carries neither those fields nor, for
// anything that does not move at all, the entity itself -- all of it is in the
// SystemLayout the client was given on entry.
//
// Both clients call this on every snapshot they accept, and afterwards the entity list is
// the whole system again. That is the point: the arrangement lives here, and nothing
// downstream -- proxies, the overview, the target panel, an agent's observation -- has to
// know that half its world arrived an hour ago.
void CompleteFromLayout(Snapshot& s, const std::map<int, EntityLayout>& layout);

std::string EncodeHello(const Hello& h);
bool        DecodeHello(const std::string& s, Hello& out);
std::string EncodeBye(const Bye& b);
bool        DecodeBye(const std::string& s, Bye& out);
std::string EncodeStarts(const Starts& s);
bool        DecodeStarts(const std::string& s, Starts& out);
std::string EncodeChooseStart(const ChooseStart& c);
bool        DecodeChooseStart(const std::string& s, ChooseStart& out);
std::string EncodeChallenge(const Challenge& c);
bool        DecodeChallenge(const std::string& s, Challenge& out);
std::string EncodeAuth(const Auth& a);
bool        DecodeAuth(const std::string& s, Auth& out);
std::string EncodeCommand(const Command& c);
bool        DecodeCommand(const std::string& s, Command& out);
// A whole snapshot, every entity in full: what a SnapshotSender sends when it has no state
// the client acknowledged, and what a test or a selftest sends when it wants no state at all.
// DecodeSnapshot reads only a whole one; a delta needs the SnapshotReceiver that holds its
// base, and is refused here rather than read as a world with most of it missing.
std::string EncodeSnapshot(const Snapshot& s);
bool        DecodeSnapshot(const std::string& s, Snapshot& out);
std::string EncodeLayout(const SystemLayout& s);
bool        DecodeLayout(const std::string& s, SystemLayout& out);
std::string EncodeLayoutDelta(const LayoutDelta& d);
bool        DecodeLayoutDelta(const std::string& s, LayoutDelta& out);
std::string EncodeDesignDef(const DesignDef& d);
bool        DecodeDesignDef(const std::string& s, DesignDef& out);
std::string EncodeGalaxy(const GalaxyState& s);
bool        DecodeGalaxy(const std::string& s, GalaxyState& out);
std::string EncodeFactions(const FactionList& f);
bool        DecodeFactions(const std::string& s, FactionList& out);
std::string EncodeChatSay(const ChatSay& c);
bool        DecodeChatSay(const std::string& s, ChatSay& out);
std::string EncodeChatLines(const ChatLines& c);
bool        DecodeChatLines(const std::string& s, ChatLines& out);
std::string EncodeChatMembers(const ChatMembers& m);
bool        DecodeChatMembers(const std::string& s, ChatMembers& out);
std::string EncodeCorpAct(const CorpAct& a);
bool        DecodeCorpAct(const std::string& s, CorpAct& out);
std::string EncodeCorpView(const CorpView& v);
bool        DecodeCorpView(const std::string& s, CorpView& out);

// The galaxy index (#206): which systems exist, their names, map positions, security and
// owners, the links between them and where a new player starts. The server sends it at
// login, before the first layout, and again whenever the index changes -- a generated
// region (#140) exists only on the server, so a client reading its own data folder would
// never see it. `SystemInfo::file` is the server's business and is not sent; a decoded
// index has it empty.
std::string EncodeUniverse(const WorldLoader::Universe& u);
bool        DecodeUniverse(const std::string& s, WorldLoader::Universe& out);

// --- Snapshot deltas and the area of interest (#349) ---
//
// A snapshot was the whole of what a player could see, twenty times a second, whether or not
// any of it had changed: about 3 KB, 59 KB/s for one idle client. Now each one is numbered,
// the client acknowledges what it has, and the server sends only what differs from the newest
// state the client acknowledged -- per entity and per field, with what is gone named.
// Quake III's scheme: the base is whatever was acknowledged, not the last snapshot sent, so
// nothing is assumed to have arrived until the client says so. With no acknowledged state
// held -- a new client, a new system, acks that stopped coming -- the snapshot is whole.
//
// Both ends hold the same state exactly, because what is held is what was on the wire:
// entities as integers (a hundredth of a unit, a milliradian, a thousandth of the hull), so a
// change is decided by comparing integers and nothing drifts between the two copies. The
// player's own ship is not quantised: prediction replays inputs on top of it, and a ship
// put back a few hundredths off every snapshot would be corrected every snapshot.

// How much of a system a viewer is told about in full. Everything within `nearRadius` of the
// ship -- or relevant to it: its target, what it holds station on, its order's object -- goes
// at every snapshot to a hundredth of a unit, up to `nearCap` things, nearest first. The rest
// of the system is still there, because the overview and the sensor screen list it, but as a
// coarse far list: positions to `farGrid` units, each thing refreshed once in `farRefresh`
// seconds. The radius is far beyond any weapon or aggression range in the game, so nothing
// that can hurt a player is ever on the far list.
struct Interest
{
    float  nearRadius = 30000.0f;
    int    nearCap = 96;
    double farRefresh = 1.0;
    float  farGrid = 10.0f;
};

// What a client says it has: the number of the newest snapshot it rebuilt. Sent once a frame
// in which one arrived; the server bases the next delta on it.
struct Ack
{
    int snapshot = 0;
};
std::string EncodeAck(const Ack& a);
bool        DecodeAck(const std::string& s, Ack& out);

// What one snapshot left a client holding, and one entity of it; Protocol.cpp's business.
struct SnapshotState;
struct EntityWire;

// The server's half, one per connection.
class SnapshotSender
{
public:
    explicit SnapshotSender(Interest interest = Interest{}) : interest_(interest) {}

    // `s` encoded for this client: a delta against the newest state it acknowledged if that is
    // still held, otherwise whole. `relevant` are ids sent in full wherever they are. Cover is
    // not decided here: what `s` does not carry, the client is not told.
    std::string Encode(const Snapshot& s, const std::set<int>& relevant);
    // The client has rebuilt snapshot `n`. An older or unknown number is ignored.
    void Acknowledge(int n);
    // The base the last Encode used; 0 -- it was whole.
    int LastBase() const { return lastBase_; }
    // Whether a snapshot is due at `now` (seconds, any steady clock), and if so counts it as
    // sent then (#411). A client that has acknowledged nothing for STALL_AFTER snapshots is
    // not reading -- an agent thinking between tool calls, a link that has stalled -- and is
    // sent one a second until it acknowledges again, rather than twenty that pile up in the
    // send backlog until the connection is dropped. Unacknowledged they would be whole ones
    // too, so the pile grows at full price.
    bool Due(double now);
    // Snapshots sent past the newest acknowledged one.
    int Unacknowledged() const { return next_ - acked_; }

    // How many sent states are held for a late ack: a second and a half at 20 Hz. An ack
    // older than that is answered with a whole snapshot.
    static constexpr size_t HELD = 32;
    // Two seconds of snapshots at 20 Hz without an acknowledgement; then one a second.
    static constexpr int    STALL_AFTER = 40;
    static constexpr double STALLED_INTERVAL = 1.0;

private:
    struct Far
    {
        std::shared_ptr<const EntityWire> wire;  // the entity as last refreshed
        double                            at = 0.0;
    };
    Interest                                            interest_;
    int                                                 next_ = 0;
    int                                                 acked_ = 0;
    int                                                 lastBase_ = 0;
    double                                              lastDue_ = -1e300;
    std::map<int, std::shared_ptr<const SnapshotState>> sent_;
    std::map<int, Far>                                  far_;
};

// The client's half: rebuilds each snapshot onto the state it names.
class SnapshotReceiver
{
public:
    // A snapshot message, whole or a delta. False -- `out` untouched -- for a broken message
    // or a delta against a state not held; the server sends a whole one once the
    // acknowledgements stop moving, so a refusal heals itself.
    bool Decode(const std::string& msg, Snapshot& out);
    // The newest snapshot rebuilt: what to acknowledge. 0 -- none yet.
    int Latest() const { return latest_; }

private:
    int                                                 latest_ = 0;
    std::map<int, std::shared_ptr<const SnapshotState>> held_;
};

// Message type from the "t" field ("cmd"/"snap"/"layout"/"ldelta"/"design"/"galaxy"/
// "universe"/"factions"/"say"/"chat"/"members"); "" —
// broken/unknown. For dispatching incoming transport messages on the client side. Reports the type
// regardless of version, so a mismatch can be diagnosed rather than looking like garbage.
std::string MessageType(const std::string& s);

// Protocol version a message was written with; 0 if it is broken or carries none.
// Use it to tell "the peer speaks a different version" apart from "the message is junk".
int MessageVersion(const std::string& s);

}  // namespace Proto
