#pragma once

#include "core/WorldLoader.h"
#include "sim/SystemState.h"
#include "sim/Protocol.h"
#include "sim/Events.h"
#include "sim/FactionMind.h"
#include "sim/Orders.h"
#include "sim/ClientSession.h"
#include "sim/SaveSchema.h"
#include "sim/PlayerStep.h"
#include "player/Player.h"
#include "missions/MissionSystem.h"
#include <array>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <tuple>
#include <vector>

class NpcShip;
class Combatant;
class Ship;
struct ShipStats;
struct Blueprint;
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
    // The galaxy as players know it (#144): every system somebody has been to, and the
    // systems a gate from one of those leads to -- located and designated, uncharted.
    // Nothing further. Knowledge is shared: the first ship in charts it for everyone.
    WorldLoader::Universe KnownUniverse() const;
    // The first ship into a system beyond the wormhole may name it, once (#145). Says why
    // not in the player's journal when it cannot.
    bool NameSystem(ClientSession& s, const std::string& name);
    // Set when a system is charted; whoever sends the index takes it and resends.
    bool TakeChartsChanged()
    {
        const bool c = chartsChanged_;
        chartsChanged_ = false;
        return c;
    }

    // The region beyond the wormhole (#140): generated from `seed` and hung off the start
    // system by one wormhole gate. Call after LoadUniverse and before InitGalaxy/LoadWorld
    // -- its systems have to be in the index before anything is made for them. Reads the
    // start system's own file, so the wormhole is not placed in a planet's path.
    // `systems` overrides how many systems the region has (0: the generator's own number);
    // only a benchmark asks for another, since a saved world records the seed and not this.
    void AttachRegion(uint64_t seed, const std::string& systemsDir, int systems = 0);
    bool HasRegion() const { return hasRegion_; }
    // The generated systems as the loader reads them, by id (#140) -- for tools.
    const std::map<std::string, std::string>& RegionDocuments() const { return regionDocs_; }
    uint64_t                                  RegionSeed() const { return regionSeed_; }

    // The seed and rules a saved world was generated with, read without loading it. False
    // when the file is missing or has none (a save from before #140).
    static bool ReadWorldSeed(const std::string& path, uint64_t& seed, int& generator);

    // Creates cold aggregates for ALL galaxy systems (call after LoadUniverse).
    // Systems start "living" right away, before the player even visits.
    void InitGalaxy();

    // M0 macro on REAL numbers: security/economy drift (from current populations,
    // which Game recounts into the aggregate), economy diffusion along gate lines,
    // and territory controller changes. Does NOT touch the population (it is real;
    // maintained by the spawn director on the Game side).
    void StepWorldMacro();

    // Factions acting on the galaxy (#231): presence grows where a faction holds, reaches
    // one neighbour per period where value outweighs risk, and decides who holds what.
    struct SystemOffer
    {
        float                  traffic = 0.0f, ore = 0.0f, salvage = 0.0f;  // each 0..1
        std::vector<FactionId> defenders;  // owners of defensive stations
    };
    // What a system offers as it is -- the truth, which no faction reads directly any more
    // (#295): it is what a faction sees when it looks.
    SystemOffer OfferOf(const SystemState& st) const;  // reads st.profile, never the entities
    // What a system offers as a faction last saw it.
    static SystemOffer OfferOf(const Intel& seen);
    // What anyone looking at a system now would see, stamped with the time.
    Intel Observe(const SystemState& st) const;
    // What a faction knows (#295): its holdings, where its ships are, and what its surveys
    // brought back, each as of when it was seen.
    const FactionMind& MindOf(FactionId f) const { return minds_[(int)f]; }
    // ...and for a test, to plant a belief or a stock. What is planted before the first
    // macro pass stands; the seeding only adds what is missing.
    FactionMind& MindOf(FactionId f) { return minds_[(int)f]; }
    // Surveys and settlements under way, by id (#295).
    const std::map<int, Plan>& Plans() const { return plans_; }
    // What an outpost costs a faction's stock (#295): its blueprint's cost, counted as one
    // number. Zero when there is no outpost blueprint, which means no faction can settle.
    static float OutpostCost();
    // The world's history, oldest first, capped (#295). Everything the news feed says is
    // here too, with a time and a sequence number; surveys are here and not in the feed.
    const std::vector<ChronicleEntry>& Chronicle() const { return chronicle_; }
    // Counts what a system's static layer holds (#295). Called when the layer is built
    // and whenever it changes; see SystemProfile.
    static SystemProfile ProfileOf(const SystemState& st);
    void                 StepFactions();
    void                 StepControl(bool settling);
    static void          SeedPresence(SystemAggregate& a);
    static constexpr int ContestPasses() { return CONTEST_PASSES; }

    // System neighbors by gate lines (for macro and the spawn director), in the order the
    // links are listed. Read from an index built when the links change (#295) -- the
    // faction step asks this for every holding of every faction.
    const std::vector<std::string>& Neighbors(const std::string& id) const;

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

    // Fixed defences (#193): anything `defensive` that belongs to a faction -- today, a
    // station -- fires at the nearest hostile ship within its radius plus its range, once
    // a second, for its archetype's damage per second. Hostile means what it means to an
    // NPC of that faction: a player by their own account, an NPC by the relation matrix.
    // A hidden or docked player is not in `players`, so is not shot at.
    void StepStationDefence(SystemState& st, const std::vector<PlayerPresence>& players,
                            std::vector<FireEvent>* fires, float dt);

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
    //
    // `cost`, when given, is added to: where the time went, for `econserver macrobench`
    // (#295). Measuring changes nothing about what is done.
    struct MaintainCost
    {
        double structures = 0.0;  // StepStructures, every tick (seconds of wall time)
        double recount = 0.0;     // recounting populations into the aggregates
        double macro = 0.0;       // StepWorldMacro: drift, diffusion, factions, control
        double topUp = 0.0;       // the spawn director in every system
        int    passes = 0;        // coarse passes run
    };
    void MaintainWorld(float dt, MaintainCost* cost = nullptr);

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

    // --- Authoritative world mutation (#38) ---
    // The static layer of a system -- what the layout describes -- can change while people
    // are in it. These are the only ways it does, and each records what changed so the
    // next LayoutDelta tells everyone standing there. Not to be called from inside a pass
    // over the system's entities: adding or removing one moves the others.
    //
    // A structure, a belt, a cloud or a wreck may come and go. A star, a planet and a gate
    // may not, yet: bodies are the generator's and satellites name their planet by its
    // place in the file (#210), and a gate is a link in the galaxy's route graph.
    static bool IsMutableKind(EntityKind k);
    // Puts an object into a system on behalf of `owner` (an account name; empty for the
    // world itself) and gives it an id. Returns the id, or 0 when refused: no such system,
    // or a kind that may not be added.
    int AddStatic(const std::string& systemId, std::unique_ptr<Entity> e, const std::string& owner);
    // Takes one out. Anyone docked at it is undocked first and told why -- the station
    // they were inside no longer exists. False when there is no such object or it may
    // not be removed.
    bool RemoveStatic(const std::string& systemId, int id);
    // Something the layout carries about an object changed in place -- a wreck was
    // searched. The caller has already changed it; this only says so.
    void MarkStaticChanged(SystemState& st, int id);
    // Everything that changed since the last call, one delta per system that changed,
    // each describing its objects as they are now. The host sends these BEFORE building
    // the snapshots that follow, which is the whole ordering guarantee: a snapshot never
    // reaches a client ahead of the change it reflects.
    std::vector<Proto::LayoutDelta> TakeLayoutDeltas();
    // The key a save knows an object by (SystemState::keys), or empty for one it does not
    // keep: a ship, a body, a gate, or something put into the system by hand.
    std::string StaticKey(const std::string& systemId, int id) const;

    // --- Construction (#39) ---
    // Why this player may not lay down `bp` at `at` now, in words they can act on, or empty
    // when they may: undocked and not warping, within the blueprint's reach, clear of
    // bodies and the paths they sweep, clear of what others use, under the caps, and with
    // the cost in the hold. Shared by Deploy and the tests, so both say the same thing.
    std::string PlacementProblem(const ClientSession& s, const Blueprint& bp, Vector2 at) const;
    // Lays down a construction site of `blueprint` at `at`, named `name` (empty -- the
    // blueprint's name), owned by the session's account. Takes the cost from the hold and
    // answers in the journal either way. Returns the site's id, or 0 when refused.
    int Deploy(ClientSession& s, const std::string& blueprint, Vector2 at, const std::string& name);
    // Why `bp` may not stand at `at` in this system, or empty when it may: inside the
    // system, clear of bodies and the paths they sweep, clear of what others use, and
    // under the blueprint's cap per system. The part of PlacementProblem that is about the
    // place rather than the builder; a faction's outpost is placed by it too (#295).
    std::string SpotProblem(const SystemState& st, const Blueprint& bp, Vector2 at) const;
    // Finishes every site whose time has come and takes away every structure whose time is
    // up. Each is an instant fixed when the site went down -- nothing is integrated -- so a
    // restart in between changes nothing. Called by MaintainWorld every tick, and looks at
    // nothing that is not due (#295): the instants wait in a queue, filled when a structure
    // is added and when the static layers are built.
    void StepStructures();
    // How many structures this account has standing, in the whole galaxy.
    int StructuresOwnedBy(const std::string& account) const;

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

    // Once per world tick, after the world has moved: what the player's ship is attached
    // to (#298). A docked ship is carried to its berth beside the station, wherever the
    // station has gone on its orbit (#210), and a ship holding station on something has
    // that something's velocity measured, for a follow to match. Commands arrive in bursts
    // and an agent sends almost none, so neither can be left to the command path.
    void StepPlayerAttachment(ClientSession& s, float dt);

    // Player weapon range lives in sim/PlayerStep.h — a single source shared with the
    // client, which draws the targeting circle from it.
    static constexpr float PLAYER_WEAPON_RANGE = Sim::PLAYER_WEAPON_RANGE;

    // Facts of the player's combat: the server applies damage and credits reputation, bounty
    // and mission progress to the session's account itself; these are reported so the client
    // can draw the consequences from facts rather than recompute them.
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
        int minedUnits = 0;  // ore units extracted
    };
    // Server player mining: if the module is on and there is ore in range — extracts it
    // into the ship's cargo. miningBonus — the skill multiplier, read by the caller from the
    // session's account.
    PlayerMiningResult StepPlayerMining(ClientSession& s, SystemState& st, float miningBonus,
                                        float dt);

    // Jump: if the gate (gateId) is in the system and the player is near — returns the
    // destination system id, otherwise "". Only the check: the caller moves the session
    // with ServerEnterSystem.
    std::string JumpGateDestIfNear(const ClientSession& s, SystemState& st, int gateId) const;

    // Loot a derelict (derelictId): if near and not looted — marks it looted (a server
    // world mutation), credits the reward to the session's account and returns it;
    // otherwise 0.
    double StepPlayerLoot(ClientSession& s, SystemState& st, int derelictId);

    // Sell cargo on the system market (server mutation: market + cargo). amount is clamped
    // to cargo. Returns what was actually sold and the gross revenue at the current price
    // (the price sags in the process). The trading-skill and reputation multipliers, and
    // crediting the money, xp and reputation, happen here too, on the session's account.
    struct PlayerSellResult
    {
        int    sold = 0;
        double gross = 0.0;    // gross revenue at the market price (before multipliers)
        double revenue = 0.0;  // net revenue credited to account_ (skill+reputation)
    };
    PlayerSellResult StepPlayerSell(ClientSession& s, SystemState& st, int resourceType,
                                    int amount);

    // Switch to another ship this account owns. Refuses one it does not: the stats are
    // looked up from the catalog here rather than taken from the caller, so "refit me to
    // the best ship" is a request the server can say no to (#5). Refuses a hull whose hold
    // is smaller than the cargo carried, as BuyShip does (#219).
    bool SwitchShip(ClientSession& s, int catalogIndex);

    // Server-authoritative docking (M4e-3b). StepPlayerDock finds the nearest station of
    // st within docking range and (if the player is not warping) fixes the docking, returning
    // the station id (0 — failed). While docked, the player's physics is not stepped.
    // Reputation admittance is decided here as well: a station whose faction hates the
    // player refuses. Undock releases the docking.
    //
    // Docked means attached (#298): the ship is berthed just outside the hull on the side it
    // came in from, carried there every tick by StepPlayerAttachment, and undocking leaves
    // it at that berth -- beside where the station is now, not where it was at the dock.
    int  StepPlayerDock(ClientSession& s, SystemState& st);
    void StepPlayerUndock(ClientSession& s);

    // --- Player account (server-authoritative, M4f) ---
    // Money/skills/reputation/wanted live on the session (ClientSession::account) and are
    // applied inside these server steps; the client shows a mirror of them taken from the
    // snapshot. The journal of what happened is per session too.
    //
    // Passive per tick: piloting xp (in flight, not docked) + bounty decay.
    void StepPlayerAccountTick(ClientSession& s, float dt);
    // Pay off a bounty with a faction (deducts the bounty from money, zeroes the wanted level).
    // The hangar, bounty and mission verbs answer in the session's journal as well as in the
    // return value (#219): a Notice on success and on refusal, with the reason, so a human
    // sees why a button did nothing and an agent's wait_for_event wakes on it.
    bool PayBounty(ClientSession& s, FactionId faction);
    // Buy a ship by catalog index: price accounting for the docked station's reputation;
    // if affordable — deducts and refits. Refuses a ship already owned: going back to one is
    // SwitchShip, and free. true — the purchase succeeded.
    bool BuyShip(ClientSession& s, int catalogIndex);
    // Whether a faction is hostile to this player by account (pirates; wanted;
    // Hostile/Hated reputation) — the server combat predicate.
    bool AccountHostileToFaction(const ClientSession& s, FactionId f) const;

    // --- Player missions (server-authoritative, M4f-2) ---
    // Generates the offer board at the docked station (called from StepPlayerDock).
    void GenerateDockOffers(ClientSession& s);
    // Takes an offer from the board, unless the session already carries
    // MissionSystem::MAX_ACTIVE missions. true -- taken.
    bool AcceptMission(ClientSession& s, int offerIndex);
    // Server-side mission hand-in: checks the condition, credits rewards to the account,
    // removes cargo (Mining), deletes from the log. true — handed in.
    bool CompleteMission(ClientSession& s, int activeIndex);
    // Hand-in condition at the station this player is docked at.
    bool MissionCompletableNow(const ClientSession& s, const Mission& m) const;

    // Server-side active-system change (for the headless host on a jump): makes destId
    // active and teleports the player to the gate leading back to fromId (as the arrival
    // point). Systems are already materialized, so no hydrate is needed.
    // fromId is by value on purpose: callers pass s.systemId, which this overwrites (#310).
    void ServerEnterSystem(ClientSession& s, const std::string& destId, std::string fromId);

    // Where a ship coming from fromId appears in destId (#310): beside the gate whose
    // destination is fromId, ARRIVAL_CLEARANCE beyond its edge towards the middle of the
    // system, and (through heading, when given) facing away from it. Without such a gate,
    // SafeArrival.
    static constexpr float ARRIVAL_CLEARANCE = 200.0f;
    Vector2 ArrivalFrom(const std::string& destId, const std::string& fromId,
                        const ClientSession* who = nullptr, float* heading = nullptr) const;

    // How close a body must be for a saved ship to be kept beside it rather than at a point
    // in space (#258): within this of its surface.
    static constexpr float NEAR_BODY_RANGE = 50000.0f;

    // Where a ship that has no better place to be appears in a system: beside its first
    // station, inside docking reach, so a new player's first choice is to dock or to
    // fly. A fixed point would do until the bodies grow (#159) -- then any constant lands
    // inside a star somewhere.
    // Where a ship appears without a gate: beside a station that would take this player,
    // else clear of every gun that would fire on them and of the stars (#224). Without a
    // session, beside the first station.
    Vector2 SafeArrival(const std::string& systemId, const ClientSession* who = nullptr) const;

    double Time() const { return time_; }
    void   SetTime(double t) { time_ = t; }

    // WORLD persistence (server-side): the galaxy (system aggregates) + time into a
    // separate world.json file. Does not touch entities — after LoadWorld the caller
    // materializes the world (MaterializeAllSystems). The player account is NOT included.
    // What players changed in the static layer goes with it (#38): per system, the keys of
    // the world's own objects that were taken away, the state of those that changed, and
    // every object added since, whole, in the format of a system document.
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
    // A system's static layer was just built from its document: give the objects that may
    // change their keys, then replay what the save said had changed (#38).
    void KeyWorldObjects(SystemState& st);
    void ReplayChanges(SystemState& st);
    // Rebuilds neighbors_ from universe_.links; called wherever the links change.
    void IndexLinks();

    WorldLoader::Universe              universe_;
    bool                               hasRegion_ = false;
    uint64_t                           regionSeed_ = 0;
    std::map<std::string, std::string> regionDocs_;  // generated system documents, as JSON (#140)
    std::string                        wormhole_;    // the gate added to the start system, as JSON
    std::map<std::string, SystemState> systems_;     // state by system id
    int                                agentIdCounter_ = 0;
    // The links, by system (#295): what Neighbors reads.
    std::map<std::string, std::vector<std::string>> neighbors_;

    // One per connected player (#3). A std::map because the verbs take a ClientSession&,
    // and a session must not move under one while the world is being stepped.
    std::map<int, ClientSession> sessions_;
    int                          sessionIdCounter_ = 0;

    double time_ = 0.0;        // total simulation time (seconds)
    double maintAccum_ = 0.0;  // accumulator of coarse world maintenance (director)
    // Macro passes since this process started; the first few are the world settling and
    // are not news (#143).
    int macroSteps_ = 0;
    int factionPasses_ = 0;  // macro passes counted by StepFactions (#231)
    // How many macro passes in a row a side must hold a system before it changes hands
    // (#225): three minutes, long enough for players to notice and answer.
    static constexpr int CONTEST_PASSES = 90;
    bool                 chartsChanged_ = false;  // a system was charted (#144)
    static constexpr int SETTLE_STEPS = 3;
    unsigned int         rng_ = 0x1234567u;  // RNG state

    std::vector<std::string> events_;  // recent galaxy events (capped)

    // What each faction knows, and what it has set in motion (#295). Saved. Seeded with
    // its holdings and their neighbours the first time the faction step runs, when the
    // static layers they are read from exist -- or, for an older save, the first time
    // after it is loaded.
    std::array<FactionMind, FACTION_COUNT> minds_;
    bool                                   mindsSeeded_ = false;
    std::map<int, Plan>                    plans_;
    // Plans by when they are due, then by id: a pass pops what is due and looks at
    // nothing else. Not saved -- remade from plans_.
    std::set<std::pair<double, int>> due_;
    int                              nextPlanId_ = 1;
    std::vector<ChronicleEntry>      chronicle_;
    long long                        chronicleSeq_ = 0;

    // When each structure next has something happen to it -- it is finished, or its time is
    // up -- as (instant, entity id, system). StepStructures pops what is due and touches
    // nothing else (#295). Not saved: remade when the static layers are built, from the time
    // lines the structures carry. An entry whose structure has gone is skipped.
    std::set<std::tuple<double, int, std::string>> structureDue_;
    // Puts a structure's coming instants on the queue; anything else is ignored. Whatever
    // changes a structure's time line after it is added schedules it again.
    void ScheduleStructure(const std::string& systemId, const Entity& e);

    void SeedMinds();
    void Think(FactionId f);
    void ResolveDuePlans();
    void ResolveSurvey(const Plan& p);
    // A faction lays an outpost site in `to`, coming from `from`, and pays for it (#295).
    // False when the system turns out not to be one it may settle, or there is no room.
    bool Settle(FactionId f, const std::string& from, const std::string& to);
    void ResolveSettle(const Plan& p);
    // `f` takes a system: its controller, and the security a change of hands brings.
    static void TakeControl(SystemAggregate& a, FactionId f);
    void        Record(const std::string& kind, int faction, const std::string& system,
                       const std::string& text);
    // News: the feed every client shows, and the history, which keeps who and where.
    void        Announce(const std::string& kind, int faction, const std::string& system,
                         const std::string& text);
    std::string DescribeSurvey(const Intel& seen, FactionId by) const;

    void        SeedAggregate(SystemState& st, const WorldLoader::SystemInfo& info);
    std::string SystemName(const std::string& id) const;
    void        PushEvent(const std::string& msg);
};
