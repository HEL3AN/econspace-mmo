#pragma once

#include "core/WorldLoader.h"
#include "sim/SystemState.h"
#include "sim/Protocol.h"
#include "sim/Events.h"
#include "sim/Orders.h"
#include "sim/ClientSession.h"
#include "sim/SaveSchema.h"
#include "sim/PlayerStep.h"
#include "player/Player.h"
#include "missions/MissionSystem.h"
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

class NpcShip;
class Combatant;
class Ship;
struct ShipStats;
enum class NpcRole;

// Authoritative galaxy simulation. Owns the state of systems and the galaxy index;
// Game accesses the world only through it. This is the seam for the future split
// of "server (simulation) <-> client (render/input)".
//
// L0: one system is active (as before). But the API deliberately does NOT assume a
// single active system — in L1+ there will be many systems, each with its own level
// of detail (fidelity), and under MMO there may be several hot systems.
class Simulation
{
public:
    Simulation();
    ~Simulation();  // out of line: the unique_ptr<Ship> member needs Ship's full type

    // Loads the galaxy index (universe.json).
    void                         LoadUniverse(const std::string& path);
    const WorldLoader::Universe& Universe() const { return universe_; }

    // Creates cold aggregates for ALL galaxy systems (call after LoadUniverse).
    // Systems start "living" right away, before the player even visits.
    void InitGalaxy();

    // M0 macro on REAL numbers: security/economy drift (from current populations,
    // which Game recounts into the aggregate), economy diffusion along gate lines,
    // and territory controller changes. Does NOT touch the population (it is real;
    // maintained by the spawn director on the Game side).
    void StepWorldMacro();

    // System neighbors by gate lines (for macro and the spawn director).
    std::vector<std::string> Neighbors(const std::string& id) const;

    // --- Step-by-step simulation of system agents (server core, M3) ---
    // Are two NPCs hostile (faction relation matrix) — pure server logic.
    static bool NpcHostileToNpc(const NpcShip* a, const NpcShip* b);

    // A player standing in a system, as the NPC passes see them. The caller builds one
    // per session in the system: whose ship it is, and whether something is hiding it.
    // Hostility is not passed in -- the account is server-side now, so the pass asks.
    struct PlayerPresence
    {
        const ClientSession* session = nullptr;
        Combatant*           ship = nullptr;
        bool                 hidden = false;  // inside something that hides ships
    };

    // System AI pass: combat roles pursue the nearest hostile target, peaceful ones
    // flee. Players in the system are targets like any other, each judged by their own
    // account -- a pirate hunts the player it is hostile to, not "the player".
    void StepNpcAi(SystemState& st, const std::vector<PlayerPresence>& players);

    // NPC combat: each ready combat NPC hits the nearest hostile target (NPC or player)
    // through the Combatant interface. fires!=nullptr — collect fire events (the client
    // turns them into beams). A hidden player is not shot at.
    void StepNpcCombat(SystemState& st, const std::vector<PlayerPresence>& players,
                       std::vector<FireEvent>* fires);

    // One full step of a system: AI, movement, combat, cleanup of the fallen. `players`
    // is whoever happens to be standing in it, which is usually nobody -- a system with
    // no one in it is the normal case, not a lesser kind of step (#3).
    void StepSystemAgents(SystemState& st, const std::vector<PlayerPresence>& players,
                          std::vector<FireEvent>* fires, float dt);

    // Server-side player respawn: teleport to the first station of the active system,
    // repair, cargo loss (like the client RespawnPlayer). No undock needed.
    void ServerRespawnPlayer(ClientSession& s);

    // --- Spawn director and coarse world maintenance (server core, M3) ---
    // System nodes for spawning by category. dangerGates — gates into dangerous
    // (low-sec/pirate) systems: only there do pirate ambushes belong, not at peaceful ones.
    struct SpawnNodes
    {
        std::vector<Vector2> stations, gates, fields, outerSpots, dangerGates;
    };
    SpawnNodes                  GatherNodes(const SystemState& st) const;
    static std::vector<Vector2> PirateSpots(const SpawnNodes& nd);
    // Pirate spawn point: offset from a node; avoid!=nullptr — push it farther from that
    // point (the player's position in the active system) so it is not right in view.
    Vector2 PirateSpawnPos(const std::vector<Vector2>& pool, const std::vector<Vector2>& avoid);

    // Creates an NPC with a stable id and puts it into the system.
    void SpawnNpcInto(SystemState& st, Vector2 pos, FactionId faction, NpcRole role,
                      std::vector<Vector2> waypoints);
    // Recounts live NPCs by role into the system aggregate.
    void RecountAgg(SystemState& st);
    // Spawn director for one system: tops the population up to targets by security/controller,
    // accounting for the "pressure" from losses. avoid — the player's position (active system) or
    // null.
    void TopUpSystem(SystemState& st, const std::vector<Vector2>& avoid);
    // Coarse world maintenance every ~2 s of simulation: recount + "pressure", macro,
    // spawn director across all systems. Where players are is read from the sessions:
    // pirates are not dropped on top of someone, in whichever systems people happen to be.
    void MaintainWorld(float dt);

    // Materializes a system from its aggregate: spawn NPCs by role in suitable places.
    void HydrateSystem(SystemState& st);
    // M0: loads the static objects of all systems from systemsDir (with a trailing '/')
    // and populates them with NPCs. Used by both the client (Game) and the server (econserver).
    void MaterializeAllSystems(const std::string& systemsDir);

    // M4: builds the WORLD snapshot of a system (entities with id/kind/position, etc.) for
    // the client. Player state and fire events are added by the caller (the client owns the
    // player ship until M4f).
    Proto::Snapshot BuildSnapshot(const ClientSession& s, const std::string& systemId) const;

    // M4d-3c: builds the static layout of a system (only stationary objects —
    // star/planets/stations/fields/gates/nebulae/derelicts) for building the
    // client world proxy without access to the server's live entities. NPCs are not
    // included here (they are snapshot dynamics).
    Proto::SystemLayout BuildLayout(const std::string& systemId) const;

    // M4e-3c: galaxy snapshot (statistics of all systems + news) for the networked
    // client's galaxy map. Not const: refreshes the aggregates' population (RecountAgg).
    Proto::GalaxyState BuildGalaxyState();

    // --- Players as server agents (M4d-2b, #3) ---
    // The simulation owns every connected player's ship and account, one ClientSession
    // each, and steps them from their own commands. The verbs below are the rules; which
    // player they act for is an argument, not a member.
    ClientSession& CreateSession(const std::string& systemId, Vector2 pos, const ShipStats& stats);
    void           DestroySession(int id);
    ClientSession* Session(int id);  // nullptr if there is no such session
    const ClientSession* Session(int id) const;

    std::map<int, ClientSession>&       Sessions() { return sessions_; }
    const std::map<int, ClientSession>& Sessions() const { return sessions_; }

    // Server step of a player ship: applies the movement axes/toggles from the command,
    // the piloting bonus, and updates the physics. Combat/mining — via separate methods.
    void StepPlayerShip(ClientSession& s, const Proto::Command& cmd, float pilotBonus, float dt);

    // Player weapon range lives in sim/PlayerStep.h — a single source shared with the
    // client, which draws the targeting circle from it.
    static constexpr float PLAYER_WEAPON_RANGE = Sim::PLAYER_WEAPON_RANGE;

    // Facts of the player's combat for the client account: the server applies damage and
    // reports what happened; the client credits reputation/bounty/mission credit (account until
    // M4f).
    struct PlayerCombatEvents
    {
        bool      hitLawful = false;  // hit a lawful target (a crime)
        FactionId hitFaction = FactionId::Independent;
        bool      killedPirate = false;  // killed a pirate (mission credit)
        bool      killedLawful = false;  // killed a lawful target (a serious crime)
        FactionId killedFaction = FactionId::Independent;
        Vector2   shotFrom = { 0.0f, 0.0f };  // player's shot beam (for render/snapshot)
        Vector2   shotTo = { 0.0f, 0.0f };
    };
    // Server player combat: if the weapon is on and the target (targetId) is in range and
    // the cooldown is ready — applies damage, accumulates facts in ev. Returns true if there
    // was a shot this tick (the client draws a beam). The target is looked up in st by id.
    // Shortest path across the gate graph, as system ids from `from` to `to` inclusive.
    // Empty when there is no route, or when either end is unknown.
    //
    // avoidDanger weighs each hop by how dangerous the system is (low security, pirate
    // pressure, a hostile controller) instead of counting hops. "Fastest" and "arrives"
    // are different routes, and which one an agent wants depends on what it is carrying.
    std::vector<std::string> PlanRoute(const std::string& from, const std::string& to,
                                       bool avoidDanger) const;

    // --- Standing orders (strategic layer, #26) ---
    // Give an order, replacing whatever was running; returns its id. The order executes
    // over seconds on the server while the tactical loop keeps running at its own rate.
    int  GiveOrder(ClientSession& s, const Orders::Order& o);
    void AbortOrder(ClientSession& s, const std::string& why);
    // Advances the running order by one tick. It decides what this tick's command should
    // be and drives the ship through the same path a client command takes, so predicted
    // and ordered movement cannot diverge.
    void StepPlayerOrder(ClientSession& s, SystemState& st, float dt);

    // Weapon state is server-owned, like docking: the client sends a toggle intent and
    // reads the truth back from the snapshot. Two independent copies would drift apart on
    // any dropped or duplicated command, leaving the reticle disagreeing with the guns.
    // The bit itself lives on the session (ClientSession::ToggleWeapon).
    bool StepPlayerFire(ClientSession& s, SystemState& st, int targetId, float dt,
                        PlayerCombatEvents* ev);

    // Result of the server player mining for a tick.
    struct PlayerMiningResult
    {
        int fieldId = 0;     // the field being mined (0 — none; for the beam)
        int minedUnits = 0;  // ore units extracted (for xp on the client)
    };
    // Server player mining: if the module is on and there is ore in range — extracts it
    // into the ship's cargo. miningBonus — the skill multiplier (account, passed by the client).
    PlayerMiningResult StepPlayerMining(ClientSession& s, SystemState& st, float miningBonus,
                                        float dt);

    // Jump: if the gate (gateId) is in the system and the player is near — returns the
    // destination system id, otherwise "". The system change itself is done by the client
    // (layout still local); the server only validates proximity to the gate.
    std::string JumpGateDestIfNear(const ClientSession& s, SystemState& st, int gateId) const;

    // Loot a derelict (derelictId): if near and not looted — marks it looted (a server
    // world mutation) and returns the reward; otherwise 0. The money is credited by the
    // client (account).
    double StepPlayerLoot(ClientSession& s, SystemState& st, int derelictId);

    // Sell cargo on the system market (server mutation: market + cargo). amount is clamped
    // to cargo. Returns what was actually sold and the gross revenue at the current price
    // (the price sags in the process). Skill/reputation multipliers and crediting of
    // money/xp/reputation — on the client side (account until M4f).
    struct PlayerSellResult
    {
        int    sold = 0;
        double gross = 0.0;    // gross revenue at the market price (before multipliers)
        double revenue = 0.0;  // net revenue credited to account_ (skill+reputation)
    };
    PlayerSellResult StepPlayerSell(ClientSession& s, SystemState& st, int resourceType,
                                    int amount);

    // Refit the player ship to different stats (station hangar). A server mutation of the
    // ship; ship ownership/money/index — on the client side (account).
    // Switch to another ship this account owns. Refuses one it does not: the stats are
    // looked up from the catalog here rather than taken from the caller, so "refit me to
    // the best ship" is a request the server can say no to (#5).
    bool SwitchShip(ClientSession& s, int catalogIndex);

    // Server-authoritative docking (M4e-3b). StepPlayerDock finds the nearest station of
    // st within docking range and (if the player is not warping) fixes the docking, returning
    // the station id (0 — failed). While docked, the host freezes the player's physics.
    // Reputation admittance — on the client side (account). Undock releases the docking.
    int  StepPlayerDock(ClientSession& s, SystemState& st);
    void StepPlayerUndock(ClientSession& s)
    {
        if (s.dockedStationId != 0)
            s.RecordEvent(Ev::Kind::Undocked, "Undocked");
        s.dockedStationId = 0;
    }

    // --- Player account (server-authoritative, M4f) ---
    // Money/skills/reputation/wanted live on the session (ClientSession::account) and are
    // applied inside these server steps; the client shows a mirror of them taken from the
    // snapshot. The journal of what happened is per session too.
    //
    // Passive per tick: piloting xp (in flight, not docked) + bounty decay.
    void StepPlayerAccountTick(ClientSession& s, float dt);
    // Pay off a bounty with a faction (deducts the bounty from money, zeroes the wanted level).
    void PayBounty(ClientSession& s, FactionId faction);
    // Buy a ship by catalog index: price accounting for the docked station's reputation;
    // if affordable — deducts and refits. true — the purchase succeeded.
    bool BuyShip(ClientSession& s, int catalogIndex);
    // Whether a faction is hostile to this player by account (pirates; wanted;
    // Hostile/Hated reputation) — the server combat predicate.
    bool AccountHostileToFaction(const ClientSession& s, FactionId f) const;

    // --- Player missions (server-authoritative, M4f-2) ---
    // Generates the offer board at the docked station (called from StepPlayerDock).
    void GenerateDockOffers(ClientSession& s);
    // Server-side mission hand-in: checks the condition, credits rewards to the account,
    // removes cargo (Mining), deletes from the log. true — handed in.
    bool CompleteMission(ClientSession& s, int activeIndex);
    // Hand-in condition at the station this player is docked at.
    bool MissionCompletableNow(const ClientSession& s, const Mission& m) const;

    // Server-side active-system change (for the headless host on a jump): makes destId
    // active and teleports the player to the gate leading back to fromId (as the arrival
    // point). Systems are already materialized, so no hydrate is needed.
    void ServerEnterSystem(ClientSession& s, const std::string& destId, const std::string& fromId);

    double Time() const { return time_; }
    void   SetTime(double t) { time_ = t; }

    // WORLD persistence (server-side): the galaxy (system aggregates) + time into a
    // separate world.json file. Does not touch entities — after LoadWorld the caller
    // materializes the world (MaterializeAllSystems). The player account is NOT included.
    void SaveWorld(const std::string& path) const;
    // Save::Result::TooNew means the file was written by a later build: it is left alone
    // and nothing is loaded, because reading it with this build's rules would turn an
    // unknown field into a default and then write that back (#20).
    Save::Result LoadWorld(const std::string& path);

    // Account persistence, one file per account name (the world has its own world.json).
    // Money, skills, reputation and wanted levels; where the player was and what they were
    // carrying (#49); which ships they own and which one they are flying (#5). Without it
    // a session would begin from nothing every time somebody connected.
    void         SaveAccount(const ClientSession& s, const std::string& path) const;
    Save::Result LoadAccount(ClientSession& s, const std::string& path);

    // Feed of galactic events (system seizures/reconquests) — for showing to the player.
    const std::vector<std::string>& Events() const { return events_; }

    // Deterministic simulation RNG (for reproducibility/server).
    void  Seed(unsigned int s) { rng_ = s ? s : 1u; }
    int   RandRange(int lo, int hi);  // inclusive [lo, hi]
    float Rand01();

    // System persistence: a system's state lives on after the player leaves.
    bool HasSystem(const std::string& id) const { return systems_.count(id) > 0; }
    // Fully clears the galaxy (for loading a save).
    void Reset();

    // A system by id, and the system a given player is in. There is no "active" system
    // any more (#3): every system runs, and the only thing that distinguishes one is
    // which players are standing in it. nullptr for an id the galaxy does not have.
    SystemState*       SystemById(const std::string& id);
    const SystemState* SystemById(const std::string& id) const;
    SystemState*       SystemOf(const ClientSession& s) { return SystemById(s.systemId); }
    const SystemState* SystemOf(const ClientSession& s) const { return SystemById(s.systemId); }

    // The index record for a system (id/name/security/owner) or nullptr.
    const WorldLoader::SystemInfo* SystemInfoById(const std::string& id) const;

    // All systems (for save/load and background ticks).
    std::map<std::string, SystemState>&       Systems() { return systems_; }
    const std::map<std::string, SystemState>& Systems() const { return systems_; }

    // Stable agent ids: issuance and tracking of the maximum (when loading a save).
    int  NextAgentId() { return ++agentIdCounter_; }
    void ObserveAgentId(int id)
    {
        if (id > agentIdCounter_)
            agentIdCounter_ = id;
    }

private:
    WorldLoader::Universe              universe_;
    std::map<std::string, SystemState> systems_;  // state by system id
    int                                agentIdCounter_ = 0;

    // One per connected player (#3). A std::map because the verbs take a ClientSession&,
    // and a session must not move under one while the world is being stepped.
    std::map<int, ClientSession> sessions_;
    int                          sessionIdCounter_ = 0;

    double       time_ = 0.0;        // total simulation time (seconds)
    double       maintAccum_ = 0.0;  // accumulator of coarse world maintenance (director)
    unsigned int rng_ = 0x1234567u;  // RNG state

    std::vector<std::string> events_;  // recent galaxy events (capped)

    void        SeedAggregate(SystemState& st, const WorldLoader::SystemInfo& info);
    std::string SystemName(const std::string& id) const;
    void        PushEvent(const std::string& msg);
};
