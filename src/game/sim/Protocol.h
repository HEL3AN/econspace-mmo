#pragma once

#include "raylib.h"
#include "core/Faction.h"
#include "sim/Events.h"
#include "sim/SystemState.h"  // FireEvent

#include <string>
#include <vector>

// Client<->server protocol (track M, M4). The CLIENT sends `Command` (player
// intents), the SERVER returns `Snapshot` (view of the player's system). JSON
// serialization (readable, debuggable; can switch to binary later). Types are
// render-independent: role/kind stored as numbers, no protocol dependency on game classes.
namespace Proto
{

// Wire format version. Bump it whenever a message changes in a way an older build would
// misread — a renamed or repurposed field, a changed unit, a changed meaning.
//
// Every message carries it and every Decode* rejects anything else. That check is the
// whole point: per-field decoding is deliberately permissive (`value(key, default)`), so
// without it a client built against an older protocol would silently receive defaults
// instead of an error, and the failure would surface much later as a ship that does not
// move or an account that reads zero.
inline constexpr int PROTO_VERSION = 2;

// --- Command: client -> server, every tick ---
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
    // Navigation order (one-shot): mode 0 — none, 1 — autopilot, 2 — warp;
    // destination point and stop distance (autopilot) / drop distance (warp).
    int     navMode = 0;
    Vector2 navTarget = { 0.0f, 0.0f };
    float   navStopDist = 0.0f;
    int     jumpGateId = 0;  // jump through gate by id (0 — none)
    int     lootId = 0;      // loot derelict by id (0 — none)
    // Station trade (server-authoritative, M4e-3b): sell resource / refit.
    int sellType = -1;   // ResourceType as int (-1 — no sale)
    int sellAmount = 0;  // amount to sell (server clamps to cargo)
    int refitShip = -1;  // switch to an ALREADY bought ship (index; -1 none)
    // Account actions (server-authoritative, M4f): buy ship / pay bounty / cheat.
    int  buyShip = -1;           // buy ship by catalog index (-1 none)
    int  payBountyFaction = -1;  // pay off bounty with a faction (id; -1 none)
    bool debugMoney = false;     // F1: grant debug money
    int  acceptOffer = -1;       // accept mission offer by board index (-1 none)
    int  completeMission = -1;   // hand in active mission by index (-1 none)
};

// Entity kind (render-independent; the client decides how to draw it).
enum class EntityKind
{
    Unknown,
    Star,
    Planet,
    Station,
    Field,
    Gate,
    Nebula,
    Derelict,
    Npc
};

// --- Snapshot: server -> client ---
struct EntitySnapshot
{
    int         id = 0;
    EntityKind  kind = EntityKind::Unknown;
    Vector2     pos = { 0.0f, 0.0f };
    float       heading = 0.0f;
    float       size = 0.0f;
    FactionId   faction = FactionId::Independent;
    int         role = -1;        // NpcRole as int (-1 — not an NPC)
    float       hullFrac = 1.0f;  // hull fraction 0..1 (for combatants)
    int         ore = -1;         // ResourceType as int for fields (-1 — not a field)
    std::string name;
};

// Player ship state in the snapshot.
struct PlayerView
{
    Vector2 pos = { 0.0f, 0.0f };
    Vector2 vel = { 0.0f, 0.0f };
    float   heading = 0.0f;
    float   hull = 0.0f, maxHull = 0.0f, shields = 0.0f, maxShields = 0.0f;
    int     cargoUsed = 0, cargoCap = 0;
    int     dockedStationId = 0;  // docked station id (0 — in flight); server-authoritative
    int     warpPhase = 0;        // 0 none, 1 aligning, 2 warp
    // Server-authoritative navigation (warp/autopilot): the client mirrors it and keeps
    // no warp-timer scale of its own (otherwise "bar ready" and flight start diverge).
    float            warpAlign = 0.0f;  // remaining warp spin-up timer
    Vector2          warpTarget = { 0.0f, 0.0f };
    float            warpDrop = 0.0f;  // warp exit distance
    bool             autopilot = false;
    Vector2          apTarget = { 0.0f, 0.0f };
    float            apStop = 0.0f;  // autopilot stop distance
    bool             stabilizer = true;
    bool             mining = false;
    bool             weaponOn = false;
    bool             docked = false;
    int              nearbyStationId = 0;
    int              lastInput = 0;  // last input number the server processed (ack, M4e)
    std::vector<int> cargoByType;    // remaining cargo per resource (AllResourceTypes order)
    // Player account (server-authoritative, M4f): the client shows it as a mirror.
    // reputation/bounty — per faction (index = FactionId), skillXp — Piloting/Mining/Trading.
    double              money = 0.0;
    std::vector<float>  reputation;  // 4 factions
    std::vector<double> bounty;      // 4 factions
    std::vector<float>  skillXp;     // 3 skills (Piloting/Mining/Trading)
};

// Trade acknowledgment (server -> client): result of a sale. The client credits
// money/xp/reputation (account until M4f); the server computes gross (price sag).
struct TradeAck
{
    int    type = 0;       // ResourceType as int
    int    sold = 0;       // actually sold
    double gross = 0.0;    // gross revenue at the server price
    double revenue = 0.0;  // net revenue the server credited to the account (for display)
};

// Mission view in the snapshot (server-authoritative, M4f-2). Render-independent copy
// of Mission: stations addressed by stable id, completable computed by the server.
struct MissionView
{
    int         type = 0;     // MissionType as int
    int         faction = 0;  // FactionId as int
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
};

// Snapshot of the player's system — what the client draws.
struct Snapshot
{
    std::string                 systemId;
    PlayerView                  player;
    std::vector<EntitySnapshot> entities;
    std::vector<FireEvent>      fires;
    std::vector<Ev::Event>      events;         // journal entries the client has not seen (#29)
    std::vector<float>          marketPrices;   // price per resource (AllResourceTypes order)
    std::vector<TradeAck>       tradeAcks;      // one-shot trade acknowledgments (M4e-3b)
    std::vector<MissionView>    missionOffers;  // docked station board (empty outside dock)
    std::vector<MissionView>    missionActive;  // player's active missions (M4f-2)
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
    FactionId   faction = FactionId::Independent;
    int         subType = 0;         // StarType/PlanetType/StationRole as int
    float       orbitRadius = 0.0f;  // planet: orbit radius (for the ring)
    int         resource = -1;       // ResourceType: planet deposit / field ore (-1 none)
    double      reward = 0.0;        // derelict: loot reward
    std::string dest;                // gate: destination system id
};

// Full static "layout" of a system — what the client builds the world proxy from.
struct SystemLayout
{
    std::string               systemId;
    std::vector<EntityLayout> entities;
};

// Per-system statistics for the galaxy map (M4e-3c): dynamics the server computes
// (macro) — the client does not simulate them itself, so it receives them by snapshot.
struct GalaxySystemStat
{
    std::string id;
    float       security = 0.5f;
    int         pirates = 0;
    float       prosperity = 0.5f;
    FactionId   controller = FactionId::Independent;
};

// Galaxy snapshot: statistics for all systems + a news feed. Sent by the server
// periodically (about once a second) — the networked galaxy map comes alive.
struct GalaxyState
{
    std::vector<GalaxySystemStat> systems;
    std::vector<std::string>      events;
};

// JSON serialization. Decode* return false on a broken/unsuitable message.
std::string EncodeCommand(const Command& c);
bool        DecodeCommand(const std::string& s, Command& out);
std::string EncodeSnapshot(const Snapshot& s);
bool        DecodeSnapshot(const std::string& s, Snapshot& out);
std::string EncodeLayout(const SystemLayout& s);
bool        DecodeLayout(const std::string& s, SystemLayout& out);
std::string EncodeGalaxy(const GalaxyState& s);
bool        DecodeGalaxy(const std::string& s, GalaxyState& out);

// Message type from the "t" field ("cmd"/"snap"/"layout"/"galaxy"); "" — broken/unknown.
// For dispatching incoming transport messages on the client side. Reports the type
// regardless of version, so a mismatch can be diagnosed rather than looking like garbage.
std::string MessageType(const std::string& s);

// Protocol version a message was written with; 0 if it is broken or carries none.
// Use it to tell "the peer speaks a different version" apart from "the message is junk".
int MessageVersion(const std::string& s);

}  // namespace Proto
