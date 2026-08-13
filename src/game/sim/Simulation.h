#pragma once

#include "core/WorldLoader.h"
#include "sim/SystemState.h"
#include "sim/Protocol.h"
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

    // System AI pass: combat roles pursue the nearest hostile target, peaceful ones
    // flee. player!=nullptr adds the player as a target; hostileToPlayer — the client
    // predicate of hostility toward the player (reputation/wanted; empty for background).
    void StepNpcAi(SystemState& st, Combatant* player,
                   const std::function<bool(const NpcShip*)>& hostileToPlayer);

    // NPC combat: each ready combat NPC hits the nearest hostile target (NPC or player)
    // through the Combatant interface. fires!=nullptr — collect fire events (the client
    // turns them into beams); playerHidden — the player is in a nebula.
    void StepNpcCombat(SystemState& st, Combatant* player, bool playerHidden,
                       const std::function<bool(const NpcShip*)>& hostileToPlayer,
                       std::vector<FireEvent>*                    fires);

    // Full background step of a system (no player): AI + movement + combat + cleanup of the fallen.
    void StepSystemAgents(SystemState& st, float dt);

    // Step of the ACTIVE system with the player involved (server-side, M4e-3c): AI sees
    // the player, movement, NPC combat with the player as a target, collecting shots into
    // fires (the client draws beams), cleanup of the fallen. hostileToPlayer — the server
    // predicate of hostility to the player (by faction; reputation/wanted — account, until
    // M4f). playerHidden — the player is in a nebula.
    void StepActiveSystemAgents(SystemState& st, Combatant* player, bool playerHidden,
                                const std::function<bool(const NpcShip*)>& hostileToPlayer,
                                std::vector<FireEvent>* fires, float dt);

    // Server-side player respawn: teleport to the first station of the active system,
    // repair, cargo loss (like the client RespawnPlayer). No undock needed.
    void ServerRespawnPlayer();

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
    Vector2 PirateSpawnPos(const std::vector<Vector2>& pool, const Vector2* avoid);

    // Creates an NPC with a stable id and puts it into the system.
    void SpawnNpcInto(SystemState& st, Vector2 pos, FactionId faction, NpcRole role,
                      std::vector<Vector2> waypoints);
    // Recounts live NPCs by role into the system aggregate.
    void RecountAgg(SystemState& st);
    // Spawn director for one system: tops the population up to targets by security/controller,
    // accounting for the "pressure" from losses. avoid — the player's position (active system) or
    // null.
    void TopUpSystem(SystemState& st, const Vector2* avoid);
    // Coarse world maintenance every ~2 s of simulation: recount + "pressure", macro,
    // spawn director across all systems. activeId/activeAvoid — for player-avoidance
    // in the active system (in headless — empty id/nullptr).
    void MaintainWorld(float dt, const std::string& activeId, const Vector2* activeAvoid);

    // Materializes a system from its aggregate: spawn NPCs by role in suitable places.
    void HydrateSystem(SystemState& st);
    // M0: loads the static objects of all systems from systemsDir (with a trailing '/')
    // and populates them with NPCs. Used by both the client (Game) and the server (econserver).
    void MaterializeAllSystems(const std::string& systemsDir);

    // M4: builds the WORLD snapshot of a system (entities with id/kind/position, etc.) for
    // the client. Player state and fire events are added by the caller (the client owns the
    // player ship until M4f).
    Proto::Snapshot BuildSnapshot(const std::string& systemId) const;

    // M4d-3c: builds the static layout of a system (only stationary objects —
    // star/planets/stations/fields/gates/nebulae/derelicts) for building the
    // client world proxy without access to the server's live entities. NPCs are not
    // included here (they are snapshot dynamics).
    Proto::SystemLayout BuildLayout(const std::string& systemId) const;

    // M4e-3c: galaxy snapshot (statistics of all systems + news) for the networked
    // client's galaxy map. Not const: refreshes the aggregates' population (RecountAgg).
    Proto::GalaxyState BuildGalaxyState();

    // --- Player as a server agent (M4d-2b) ---
    // The simulation owns the player ship and steps its physics from the client command.
    // Creates the player ship (one per simulation; recreating resets the previous one).
    void        CreatePlayer(Vector2 pos, const ShipStats& stats);
    Ship*       PlayerShip() { return player_.get(); }  // nullptr before CreatePlayer
    const Ship* PlayerShip() const { return player_.get(); }
    // Server step of the player ship: applies the movement axes/toggles from the command,
    // the piloting bonus, and updates the physics. Combat/mining — via separate methods;
    // docking stays client-side until M4f.
    void StepPlayerShip(const Proto::Command& cmd, float pilotBonus, float dt);

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
    bool StepPlayerFire(SystemState& st, bool weaponOn, int targetId, float dt,
                        PlayerCombatEvents* ev);

    // Result of the server player mining for a tick.
    struct PlayerMiningResult
    {
        int fieldId = 0;     // the field being mined (0 — none; for the beam)
        int minedUnits = 0;  // ore units extracted (for xp on the client)
    };
    // Server player mining: if the module is on and there is ore in range — extracts it
    // into the ship's cargo. miningBonus — the skill multiplier (account, passed by the client).
    PlayerMiningResult StepPlayerMining(SystemState& st, float miningBonus, float dt);

    // Jump: if the gate (gateId) is in the system and the player is near — returns the
    // destination system id, otherwise "". The system change itself is done by the client
    // (layout still local); the server only validates proximity to the gate.
    std::string JumpGateDestIfNear(SystemState& st, int gateId) const;

    // Loot a derelict (derelictId): if near and not looted — marks it looted (a server
    // world mutation) and returns the reward; otherwise 0. The money is credited by the
    // client (account).
    double StepPlayerLoot(SystemState& st, int derelictId);

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
    PlayerSellResult StepPlayerSell(SystemState& st, int resourceType, int amount);

    // Refit the player ship to different stats (station hangar). A server mutation of the
    // ship; ship ownership/money/index — on the client side (account).
    void RefitPlayer(const ShipStats& stats);

    // Server-authoritative docking (M4e-3b). StepPlayerDock finds the nearest station of
    // st within docking range and (if the player is not warping) fixes the docking, returning
    // the station id (0 — failed). While docked, the host freezes the player's physics.
    // Reputation admittance — on the client side (account). Undock releases the docking.
    int  StepPlayerDock(SystemState& st);
    void StepPlayerUndock() { playerDockedStationId_ = 0; }
    bool IsPlayerDocked() const { return playerDockedStationId_ != 0; }
    int  PlayerDockedStationId() const { return playerDockedStationId_; }

    // --- Player account (server-authoritative, M4f) ---
    // Money/skills/reputation/wanted. Action effects are applied inside the server
    // Step methods directly in account_; the client shows it as a mirror (snapshot). In
    // single-player the account is still owned by the client (Game::player_), account_ is
    // unused — so the effects inside Step in single-player are applied to it for nothing.
    Player&       Account() { return account_; }
    const Player& Account() const { return account_; }
    // Passive per tick: piloting xp (in flight, not docked) + bounty decay.
    void StepPlayerAccountTick(float dt);
    // Pay off a bounty with a faction (deducts the bounty from money, zeroes the wanted level).
    void PayBounty(FactionId faction);
    // Buy a ship by catalog index: price accounting for the docked station's reputation;
    // if affordable — deducts and refits. true — the purchase succeeded.
    bool BuyShip(int catalogIndex);
    // Whether a faction is hostile to the player by account (pirates; wanted; Hostile/Hated
    // reputation) — the server combat predicate (analog of the client HostileToPlayerFaction).
    bool AccountHostileToFaction(FactionId f) const;

    // Queue of server notifications to the player (server->client): docking denied, etc.
    // BuildSnapshot does not touch them — the host takes them via DrainMessages() and puts
    // them into the snapshot; the client flushes each once. Unused in single-player.
    std::vector<std::string> DrainMessages()
    {
        std::vector<std::string> out;
        out.swap(outMessages_);
        return out;
    }

    // --- Player missions (server-authoritative, M4f-2) ---
    const MissionSystem& Missions() const { return missions_; }
    // Generates the offer board at the docked station (called from StepPlayerDock).
    void GenerateDockOffers();
    void AcceptMission(int offerIndex) { missions_.Accept(offerIndex); }
    // Server-side mission hand-in: checks the condition, credits rewards to account_,
    // removes cargo (Mining), deletes from the log. true — handed in.
    bool CompleteMission(int activeIndex);
    // Hand-in condition at the current docked station (account/cargo/docking — server-side).
    bool MissionCompletableNow(const Mission& m) const;

    // Server-side active-system change (for the headless host on a jump): makes destId
    // active and teleports the player to the gate leading back to fromId (as the arrival
    // point). Systems are already materialized, so no hydrate is needed.
    void ServerEnterSystem(const std::string& destId, const std::string& fromId);

    double Time() const { return time_; }
    void   SetTime(double t) { time_ = t; }

    // WORLD persistence (server-side): the galaxy (system aggregates) + time into a
    // separate world.json file. Does not touch entities — after LoadWorld the caller
    // materializes the world (MaterializeAllSystems). The player account is NOT included.
    void SaveWorld(const std::string& path) const;
    bool LoadWorld(const std::string& path);  // false — file missing/broken

    // Player ACCOUNT persistence (server-side, M4f-3): money/skills/reputation/wanted
    // into a separate account.json (the world has its own world.json). Without this the
    // server account_ is ephemeral — a server restart zeroes the progress. Key format is
    // the same as the client savegame.json. Ship type is not included yet (the server has
    // no ownership list — a separate sub-step).
    void SaveAccount(const std::string& path) const;
    bool LoadAccount(const std::string& path);  // false — file missing/broken

    // Feed of galactic events (system seizures/reconquests) — for showing to the player.
    const std::vector<std::string>& Events() const { return events_; }

    // Deterministic simulation RNG (for reproducibility/server).
    void  Seed(unsigned int s) { rng_ = s ? s : 1u; }
    int   RandRange(int lo, int hi);  // inclusive [lo, hi]
    float Rand01();

    // System persistence: a system's state lives on after the player leaves.
    bool HasSystem(const std::string& id) const { return systems_.count(id) > 0; }
    // Makes an already existing system active (without recreating/populating it).
    void Activate(const std::string& id);
    // Fully clears the galaxy (for loading a save).
    void Reset();

    bool               HasActive() const { return active_ != nullptr; }
    SystemState&       Active() { return *active_; }
    const SystemState& Active() const { return *active_; }
    const std::string& ActiveId() const { return activeId_; }

    // The index record for the active system (id/name/security/owner) or nullptr.
    const WorldLoader::SystemInfo* ActiveInfo() const;

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
    std::map<std::string, SystemState> systems_;           // state by system id
    SystemState*                       active_ = nullptr;  // pointer is stable (std::map)
    std::string                        activeId_;
    int                                agentIdCounter_ = 0;

    std::unique_ptr<Ship> player_;          // player ship (server agent, M4d-2b)
    Player account_{ 500.0 };               // player account (money/skills/reputation/wanted, M4f)
    std::vector<std::string> outMessages_;  // server notifications to the player (DrainMessages)
    MissionSystem            missions_;     // player missions (board+active, M4f-2)
    float                    playerFireTimer_ = 0.0f;       // player weapon cooldown
    float                    playerMiningProgress_ = 0.0f;  // accumulator of ore-unit fractions
    int                      playerDockedStationId_ = 0;    // docked station (0 — in flight)

    double       time_ = 0.0;        // total simulation time (seconds)
    double       maintAccum_ = 0.0;  // accumulator of coarse world maintenance (director)
    unsigned int rng_ = 0x1234567u;  // RNG state

    std::vector<std::string> events_;  // recent galaxy events (capped)

    void        SeedAggregate(SystemState& st, const WorldLoader::SystemInfo& info);
    std::string SystemName(const std::string& id) const;
    void        PushEvent(const std::string& msg);
};
