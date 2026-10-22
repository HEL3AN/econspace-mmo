#pragma once

#include "core/Route.h"
#include "core/WorldLoader.h"
#include "gen/Galaxy.h"
#include "entities/NpcShip.h"
#include "sim/SystemState.h"
#include "sim/Protocol.h"
#include "sim/Events.h"
#include "sim/FactionMind.h"
#include "sim/Orders.h"
#include "sim/ClientSession.h"
#include "sim/Corporation.h"
#include "sim/DesignStore.h"
#include "sim/SaveSchema.h"
#include "sim/WorldRules.h"
#include "sim/PlayerStep.h"
#include "player/Player.h"
#include "missions/MissionSystem.h"
#include <array>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <vector>

class NpcShip;
class Combatant;
class Ship;
class Structure;
class JumpGate;
struct ShipStats;
struct Blueprint;
namespace Gen
{
struct Region;
struct RegionParams;
struct Pack;
}  // namespace Gen
enum class NpcRole;

// Authoritative galaxy simulation. Owns the state of systems and the galaxy index;
// Game accesses the world only through it. This is the seam for the future split
// of "server (simulation) <-> client (render/input)".
//
// L0: one system is active (as before). But the API deliberately does NOT assume a
// single active system — in L1+ there will be many systems, each with its own level
// of detail (fidelity), and under MMO there may be several hot systems.
// A short market's delivery contract (#343, #407): bring `amount` of `good` to the station a
// save knows as `station` in `system` by `deadline`, for `reward` -- which `faction` set aside
// from its treasury when it posted it. On the boards until a pilot takes it. Saved.
struct Contract
{
    int          id = 0;
    FactionId    faction = Factions::Unaffiliated();
    std::string  system;
    std::string  station;  // its key in the system (SystemState::keys)
    ResourceType good;
    int          amount = 0;
    double       reward = 0.0;
    double       postedAt = 0.0, deadline = 0.0;  // world time
    bool         taken = false;
};

class Simulation
{
public:
    Simulation();
    ~Simulation();  // out of line: the unique_ptr<Ship> member needs Ship's full type

    // Loads the galaxy index (universe.json).
    void                         LoadUniverse(const std::string& path);
    const WorldLoader::Universe& Universe() const { return universe_; }
    // The public charts (#431 K1): the civilisation centres and every system a world faction
    // has charted, and the systems a gate from one of those leads to -- located and
    // designated, uncharted. What a pilot with no charts of their own sees; a pilot's own map
    // is KnownUniverseFor.
    WorldLoader::Universe KnownUniverse() const;

    // --- Knowledge (#431 slice 1; Simulation_Knowledge.cpp) ---
    // Whether a system is on the public charts.
    bool OnPublicCharts(const std::string& systemId) const;
    // Whether `s` has `systemId` charted: public, their own, or their corporation's.
    bool Knows(const ClientSession& s, const std::string& systemId) const;
    // `s` has been to `systemId`: on their own charts from now on (a "system:" key of
    // ClientSession::known), and their corporation's, unless it is public. True when it was
    // news to them.
    bool Learn(ClientSession& s, const std::string& systemId);
    // Everything on `s`'s own charts onto their corporation's: when they log in, found a
    // corporation or join one (Corps::Corp::charts).
    void ShareCharts(const ClientSession& s);
    // The system a key of ClientSession::known charts, or empty for any other key.
    static std::string ChartedSystemOf(const std::string& key);
    static std::string ChartKey(const std::string& systemId) { return "system:" + systemId; }
    // A world faction charts `systemId` (K2): it goes on the public charts, "charted by" it.
    // Called wherever a faction reaches a system -- a survey resolving, ships moving in or
    // coming to besiege, a site laid, a claim taken, a convoy arriving, a mouth anchored --
    // in a cold system as in a hot one, since all of those are the macro layer's. Nobody
    // charts what is public by its place (a centre), nor do a region's own natives chart
    // their home for the centres. If nobody from outside had been there, this is first
    // contact (K4): the natives' names become known, or an empty system takes the name the
    // faction gives it (decided after the rounds, 1), and it is news. The first faction's
    // charts stand; true when this one's were the first. A sale of charts (slice 16) is a
    // call of this for the buyer.
    bool ChartBy(const std::string& systemId, FactionId faction);
    // What the natives of `systemId`'s region call it (natives.names, #431 slice 3); empty
    // where nobody lives, or for a region sealed before natives had names.
    std::string NativeNameOf(const std::string& systemId) const;
    // The observed layer (#431 slice 14, M1). What stands in `st` now, as a summary: its
    // stations and structures, bounded, leaving out what `s` has not found (#346) -- or, for
    // a corporation's record (`s` null), anything hidden at all.
    WorldLoader::Sighting SightingOf(const SystemState& st, const ClientSession* s) const;
    // What `s` last saw standing in `systemId`, theirs or their corporation's, whichever is
    // newer; never observed when neither looked.
    WorldLoader::Sighting LastSeen(const ClientSession& s, const std::string& systemId) const;
    // Every coarse pass: wherever a pilot, a corporation member or a structure of theirs is,
    // what stands there is observed -- into the pilot's record and their corporation's -- and
    // their map is live there. Elsewhere it stays as last seen.
    void StepObservations();
    // What decides `s`'s map, as a key: equal keys, the same map. The host sends the index
    // when it differs from the one it last sent.
    std::vector<long long> KnowledgeKey(const ClientSession& s) const;
    // Changes whenever any pilot's map may have: a cheap test before asking every session.
    long long KnowledgeStamp() const
    {
        return publicRev_ + knowledgeSerial_ + (long long)corps_.Rev() + corps_.ChartsSerial();
    }
    // The first ship into a system beyond the wormhole may name it, once (#145). Only its
    // discoverer, never a later owner, and never a system its people or a faction named
    // (#431 N1, N2); and only a name NameProblem passes (N3). Says why not in the player's
    // journal when it cannot.
    bool NameSystem(ClientSession& s, const std::string& name);
    // Why `name` may not be a system's name (#431 N3), in words for whoever chose it: its
    // shape, the word filter (built in, and the server's BlockedNames), or another system --
    // its name, designation or id, or what its natives will call it -- has it, whatever the
    // case. Empty if it may.
    std::string NameProblem(const std::string& name) const;
    // Admin moderation (#431 N3; the console's `rename`): `system` -- an id, designation or
    // name -- beyond the wormhole is called `name`, or with "-" loses the name it was given
    // and is called what it would be without one: its people's name, else its designation,
    // which its discoverer may then name again. Every map that has it sees it; it is news,
    // and its discoverer is told in their journal, at once or at their next login. `said`
    // is what to tell the admin either way.
    bool RenameSystem(const std::string& system, const std::string& name, std::string& said);
    // The server's own word filter (names_blocked.txt beside world.json), on top of the
    // built-in one; the host gives it whenever the file is read.
    void SetBlockedNames(std::vector<std::string> words) { blockedNames_ = std::move(words); }
    const std::vector<std::string>& BlockedNames() const { return blockedNames_; }
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
    // The region fills skeleton slot 0 (#342).
    void AttachRegion(uint64_t seed, const std::string& systemsDir, int systems = 0);
    // The region of skeleton slot `slot` (#342), hung off `homeId` -- a hand-written system
    // or a generated one of another region -- by a wormhole. What is in it is the same
    // whatever else is open and whenever it opens; only its place on the map follows home.
    // False, and nothing changes, when the slot is open already, home does not exist, or a
    // region of another seed is attached.
    bool AttachRegion(uint64_t seed, int slot, const std::string& homeId,
                      const std::string& systemsDir, int systems = 0);
    // --- The opening log (#342 S3; Simulation_Tide.cpp) ---
    // Every region open in the world, in the order it opened: the world is its seed plus
    // this, then the pins, then what was changed. Saved in world.json.
    struct RegionOpening
    {
        int         slot = 0;
        double      openedAt = 0.0;  // world time
        int         parent = -1;     // the slot whose mouth leads in; -1 for a centre
        std::string mouth;           // the system of the parent the wormhole is in
        // open (a centre, or a frontier the world began with: its gates stay), unstable (a
        // mouth the tide opened, until it is anchored or collapses), anchored, drifted (its
        // mouth collapsed: cut off, kept), archived (drifted away at the cap: not in the
        // galaxy), returned (archived, and since brought back as another entry) -- S4.
        std::string state = "open";
        std::string by = "world";  // world (it opened with the world), tide, admin
        std::string why;           // what the tide was made of, when it was the tide
        // The mouth (#342 S4): when the present one opened -- openedAt, or a later reopening
        // of a drifted region -- when it collapses while unstable, and whether the chronicle
        // has said it is failing.
        double mouthAt = 0.0;
        double closesAt = 0.0;
        bool   warned = false;
        // Who anchored it, as its gates are named: a faction's name or a pilot's.
        std::string anchoredBy;
        double      anchoredAt = 0.0;
        // When a player was last in it, and whether the chronicle has said it is drifting
        // away (at the cap, S4).
        double lastVisit = 0.0;
        bool   fading = false;
        // Archived: the archive it went to. Otherwise the archive it came back from; -1.
        int archive = -1;
    };
    const std::vector<RegionOpening>& OpeningLog() const { return openings_; }
    // The entry of the log that is slot `slot` now -- not one archived from it -- or null.
    const RegionOpening* OpeningOf(int slot) const;
    // The system a region's wormhole leads into: ring 1, the far side of the mouth.
    static std::string EntryOf(int slot);

    // --- Mouths and Anchors (#342 S4; Simulation_Tide.cpp) ---
    // Why this player's ship may not go through gate `g`, or empty when it may: an unstable
    // mouth lets through only ships of at most the mouth rules' mass.
    std::string MouthProblem(const ClientSession& s, const Entity& g) const;
    // The blueprint an Anchor is built from (data/blueprints.json), and what one costs a
    // faction's stock, counted as one number; zero when there is none.
    static constexpr const char* ANCHOR_BLUEPRINT = "anchor";
    static float                 AnchorCost();
    static bool                  IsAnchor(const std::string& archetype);
    // The unstable mouth in `st` whose gate's edge is within `within` of `at`, nearest first,
    // and its gate; null when there is none.
    const RegionOpening* MouthNear(const SystemState& st, Vector2 at, float within,
                                   const Entity** gate = nullptr) const;
    // The galaxy's routes changed -- a mouth collapsed or reopened, a region drifted away or
    // came back -- since the last call: the host sends everyone the index again.
    bool TakeTopologyChanged()
    {
        const bool c = topologyChanged_;
        topologyChanged_ = false;
        return c;
    }

    // --- Roaming holes (#342 S9; Simulation_Holes.cpp) ---
    // EVE's wormholes: on a schedule of epochs (WorldRules::Holes), each live region's key
    // says whether it gets one this epoch, between which two systems, when it opens, how long
    // it lives, which ships it lets through and how much mass in all. Both ends are gates of
    // `gate.roaming`, hidden until a pilot's scan finds them (#346). What an epoch decided is
    // kept -- which systems were open then is not the seed's to say again.
    struct RoamingHole
    {
        std::string id;  // "h<epoch>-<slot>": unique in the world, what a pilot knows it by
        int         epoch = 0;
        int         slot = 0;  // the region whose key made it
        std::string a, b;      // the systems it joins
        Vector2     atA{ 0.0f, 0.0f }, atB{ 0.0f, 0.0f };
        double      opensAt = 0.0, closesAt = 0.0;
        double      maxMass = 0.0;  // the heaviest ship it lets through
        double      budget = 0.0;   // the mass it takes in all before it collapses
        double      spent = 0.0;
        bool        open = false;          // its gates stand
        std::string foundBy;               // the first pilot to resolve it, for the chronicle
        int         gateA = 0, gateB = 0;  // its gates' ids this run; 0 -- not built
        std::string dressed;               // the name its gates wear now
    };
    const std::vector<RoamingHole>& RoamingHoles() const { return holes_; }
    const RoamingHole*              HoleById(const std::string& id) const;
    // The hole gate `gateId` is an end of, or null.
    const RoamingHole* HoleOfGate(int gateId) const;
    // Decides epoch `epoch`'s holes, if it has not been decided: once each, whenever asked.
    void ScheduleHoles(int epoch);
    // Admin and tests: a hole between two systems, open now, letting through ships of up to
    // `maxMass`, for `hours`, `budget` in all. Its id, or empty when either system is not.
    std::string OpenHoleNow(const std::string& a, const std::string& b, double maxMass,
                            double hours, double budget);
    // The links this pilot knows through roaming holes, each with its tail (kind "hole").
    std::vector<WorldLoader::SystemLink> HoleLinksOf(const ClientSession& s) const;
    // The galaxy index as this pilot knows it (#144, #431): the public charts, their own and
    // their corporation's, the systems a gate from any of those leads to, and the holes they
    // found -- the system at the far end too, uncharted unless they know it. The host builds
    // it only when the pilot's KnowledgeKey differs from the one it last sent.
    WorldLoader::Universe KnownUniverseFor(const ClientSession& s) const;
    // Once a pass: holes of the epoch decided, opening, re-dressed, collapsing.
    void StepHoles();
    // Into `dest` by gate `gateId`, which JumpGateDestIfNear has allowed: through a roaming
    // hole this spends its mass, arrives at its far end and tells the pilot it is known to
    // them; otherwise it is ServerEnterSystem.
    void EnterThrough(ClientSession& s, int gateId, const std::string& dest);

    // --- Signatures and the scanner (#346; Simulation_Scan.cpp) ---
    // How loud an object is to a scanner -- its archetype's `signature`, or what a site will
    // become -- or 0 for one that is not hidden.
    static float SignatureOf(const Entity& e);
    // What a pilot knows a hidden object by across a restart: "hole:<id>" for a roaming hole's
    // end, "<system>/<save key>" for anything else. Empty for one that is not hidden.
    std::string SignatureKey(const SystemState& st, const Entity& e) const;
    // A short name for a signature, the same for everyone: "KQX-417".
    static std::string SignatureCode(const std::string& key);
    // Whether `e` is hidden from this pilot: it has a signature, it is not theirs, and their
    // scans have not resolved it.
    bool HiddenFrom(const ClientSession& s, const SystemState& st, const Entity& e) const;
    // The layout of a system as this pilot may see it -- BuildLayout without what is hidden
    // from them -- and a delta cut the same way.
    Proto::SystemLayout BuildLayoutFor(const ClientSession& s, const std::string& systemId) const;
    void                ForSession(const ClientSession& s, Proto::LayoutDelta& d) const;
    // Starts a pass of the ship's scanner. False, and why in the journal, when it cannot:
    // docked, warping, or a pass already under way.
    bool StartScan(ClientSession& s);
    // Every tick: a pass interrupted, or finished -- what it resolved is the pilot's.
    void StepPlayerScan(ClientSession& s);
    // What the scanner shows this pilot in the system they are in, for the snapshot.
    Proto::ScanView ScanViewOf(const ClientSession& s) const;
    // What scans resolved since the last call, as a delta of the pilot's own: by session id.
    // The system's revision moves, so a layout sent later is newer than it.
    std::vector<std::pair<int, Proto::LayoutDelta>> TakeRevealDeltas();
    // A hidden object fired on this pilot (#415): it has given itself away, and is theirs from
    // now on as if a scan had resolved it -- in the account, and in a delta of their own that
    // the host sends before the snapshot showing the shot. Nothing for one already found.
    void RevealByFire(ClientSession& s, const SystemState& st, const Entity& e);
    // Whether either end of a beam is something this pilot has not found (#415): such a beam
    // is left out of what they are sent, as the object is.
    bool BeamHiddenFrom(const ClientSession& s, const std::string& systemId,
                        const FireEvent& f) const;

    // --- The archive (#342 S4; Simulation_Drift.cpp) ---
    // A region that drifted away at the cap: its documents as they were sealed, and what had
    // become of it -- what stood and lay in each system, the names players gave, its history.
    struct ArchivedRegion
    {
        int         id = 0;
        int         slot = 0;  // where it was
        double      archivedAt = 0.0;
        std::string text;  // the archive document, as JSON
    };
    const std::map<int, ArchivedRegion>& Archives() const { return archives_; }
    // Where archive `id` is kept under `dir`.
    static std::string ArchivePath(const std::string& dir, int id);
    // Admin, and a test: region `slot` drifts away now, if it may -- a frontier with nothing
    // opened from it and no player structure in it; whether anyone comes to it, and the cap,
    // are not asked. The archive's id, or -1 with `why`.
    int ArchiveRegionNow(int slot, std::string& why);

    // A world file's header, read before the galaxy is made from it: its seed, the rules'
    // versions, its centres, its log and its growth settings. False when the file is missing
    // or has no seed.
    struct WorldHeader
    {
        uint64_t                   seed = 0;
        int                        generator = 0;  // GENERATOR_VERSION it was made under
        int                        skeleton = 0;   // SKELETON_VERSION; 0 before S3
        int                        centres = -1;   // -1 when it does not say
        std::vector<RegionOpening> log;            // empty before S3
        int                        maxRegions = 0, maxSystems = 0;  // 0: not set
        std::vector<Gen::Woven>    packs;  // where admin packs were woven (S7)
    };
    static bool ReadWorldHeader(const std::string& path, WorldHeader& out);

    // Sealing (#342 S3). A region is written to `<dir>/<slot>.json` the moment it opens, as
    // the generator made it (before pins), and a world loaded from its log reads it from
    // there rather than remaking it: a later build's rules change only regions opened later.
    // Empty (the default): not sealed at all -- a test, a benchmark, the batch mode.
    void               SetSealDir(const std::string& dir) { sealDir_ = dir; }
    const std::string& SealDir() const { return sealDir_; }
    // A sealed region's file, read: false for one that is not, or is another world's.
    static bool ReadSealed(const std::string& path, uint64_t seed, Gen::Region& out);
    static bool ReadSealedText(const std::string& text, uint64_t seed, Gen::Region& out);
    // Where slot `slot` is sealed under `dir`.
    static std::string SealedPath(const std::string& dir, int slot);
    // The sealed file of an open slot, as text; empty when it has none.
    std::string SealedText(int slot) const;

    // A world's galaxy (#342 S2): the skeleton of `params`, every civilisation centre in it
    // and the frontier region opened beside each, with data/pins.json applied (`dataDir`).
    // Nothing hand-written is needed. Call on an empty Simulation, before InitGalaxy or
    // LoadWorld. False when the skeleton has no centre to start in.
    //
    // With a `log` (a saved world, S3) it is that world's regions instead, in the order they
    // opened, each read from its sealed file under SealDir when there is one and made
    // afresh -- and sealed -- when not. Without one, every region is made afresh and sealed.
    bool OpenGalaxy(const Gen::WorldParams& params, const std::string& dataDir,
                    const std::vector<RegionOpening>& log = {});
    // Opens skeleton slot `slot` from the open slot `from`, its wormhole in the system of
    // `from` that faces it (Gen::MouthSystem) -- how the tide will open one (S3). False when
    // `from` is not open or `slot` cannot be.
    bool OpenSlotFrom(int slot, int from, const std::string& dataDir);
    // The skeleton OpenGalaxy laid out; an empty one before it (a test world of files).
    const Gen::GalaxySkeleton& Skeleton() const { return skeleton_; }
    int                        CentreCount() const { return (int)skeleton_.centres.size(); }
    // Whether a system belongs to a civilisation centre: charted from the start and named
    // long ago, so never a discovery.
    bool InCentre(const std::string& systemId) const;
    // The natives of an open region (#342 S5): the minor faction its documents name, put in
    // the registry when the region was hung in the galaxy. The unaffiliated faction for a
    // region without any -- a centre, or one sealed before natives.
    FactionId NativesOf(int slot) const;
    // How far a system's market is from a civilisation centre (#342 S5): jumps along the
    // galaxy's links to the nearest centre system, which its prices are made from
    // (RemotenessFactor). -1 when no centre reaches it, and in a world without centres.
    int Remoteness(const std::string& systemId) const;

    // Where a pilot may begin (#342 S2): the heart of each centre, with the station they
    // arrive at and whose space it is. A new account chooses one, and so does an account
    // whose system the galaxy no longer has.
    struct StartPoint
    {
        std::string systemId;
        std::string systemName;
        std::string station;  // the heart's trade hub
        std::string owner;    // the centre's faction id
    };
    const std::vector<StartPoint>& Starts() const { return starts_; }
    // Whether `systemId` is one of them.
    bool IsStart(const std::string& systemId) const;

    // --- Admin packs (#342 S7; Simulation_Packs.cpp, the format in gen/Pack.h) ---
    // The packs data/packs/ holds, and -- for a saved world -- where it wove them (its
    // header's). Before OpenGalaxy: a new world weaves them in (region packs into free slots
    // beside the regions it begins with, system packs into its frontier), and a saved one
    // reads them only for a region that is not sealed.
    void SetPacks(std::vector<std::shared_ptr<const Gen::Pack>> packs,
                  std::vector<Gen::Woven>                       woven = {});
    // Where this world's packs went, as world.json keeps them.
    const std::vector<Gen::Woven>& WovenPacks() const { return woven_; }
    // What weaving a new world could not do, one line each. A server does not make a world
    // that has any: the world does not exist yet, so nothing is lost by stopping.
    const std::vector<std::string>& PackProblems() const { return packProblems_; }
    // How data/packs/ differs from what this world wove, one line per pack: changed since (by
    // its hash), gone, or added after the world was made. What was woven is sealed, so none
    // of it changes the world; the admin is told.
    std::vector<std::string> PackChanges() const;
    // Whether a pack has a part in region `slot`, or links to it: such a region never drifts.
    bool IsPackSlot(int slot) const { return packSlots_.count(slot) != 0; }

    // --- The tide (#342 S3; Simulation_Tide.cpp, rules in WorldRules.h) ---
    // What one region's tide is, and what it was made of since its last mouth.
    struct RegionTide
    {
        double tide = 0.0;
        double fromTime = 0.0, fromPlayers = 0.0, fromExplore = 0.0, fromSurveys = 0.0;
        int    surveys = 0;            // faction surveys of the region this period
        double lastMouthAt = -1.0e18;  // when it last opened one
    };
    const std::map<int, RegionTide>& Tides() const { return tides_; }
    // Where the tide breaks through now: threshold * (1 + open regions / maxRegions).
    double TideThreshold() const;
    // An open region with a free slot beside it in the skeleton.
    bool IsEdgeRegion(int slot) const;
    // How many young mouths the world allows (1 + players / playersPerMouth), and has.
    int MouthsAllowed() const;
    int YoungMouths() const;
    // The cap (server settings): no slot opens once the world has this many regions or
    // systems. 0 leaves one as it was; the defaults are the skeleton's size and 2000.
    void SetGrowthCap(int maxRegions, int maxSystems);
    int  MaxRegions() const { return maxRegions_; }
    int  MaxSystems() const { return maxSystems_; }
    bool AtCap() const;
    // Tide that crossed the threshold at the cap and had nowhere to go, kept for the roaming
    // holes of S9.
    double SurplusTide() const { return surplusTide_; }
    // Admin: growth stopped -- no tide gathers and no mouth opens until it is resumed. Saved.
    void PauseGrowth(bool paused) { growthPaused_ = paused; }
    bool GrowthPaused() const { return growthPaused_; }
    // Admin: open a mouth now from region `fromSlot` (-1: the edge region whose tide is
    // highest), whatever its tide, the gap and the young mouths; never past the cap. The slot
    // opened, or -1 with `why` saying why not.
    int OpenMouthNow(int fromSlot, std::string& why);
    // The slots opened while the world ran, since the last call: the host tells everyone of
    // them -- the galaxy index, then the gate's delta -- before any snapshot that needs them.
    std::vector<int> TakeOpenedRegions();
    // Admin, with the server stopped (#342 S3): seals slot `slot` afresh under this build's
    // rules, from where its log entry says it opened. Refused, with `why`, while a player
    // structure stands in it, or for a slot that is not open. The caller drops what the save
    // knew of its systems.
    bool RegenerateRegion(int slot, std::string& why);
    // The systems of an open region, in the order it lists them.
    std::vector<std::string> RegionSystems(int slot) const;

    bool HasRegion() const { return hasRegion_; }
    // The slots whose regions are attached, ascending.
    const std::set<int>& OpenSlots() const { return openSlots_; }
    // The generated systems as the loader reads them, by id (#140) -- for tools.
    const std::map<std::string, std::string>& RegionDocuments() const { return regionDocs_; }
    uint64_t                                  RegionSeed() const { return regionSeed_; }

    // The seed and rules a saved world was generated with, read without loading it. False
    // when the file is missing or has none (a save from before #140). `centres`, when
    // given, is how many civilisation centres it was made with (#342 S2), -1 if it says not.
    static bool ReadWorldSeed(const std::string& path, uint64_t& seed, int& generator,
                              int* centres = nullptr);

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
    const FactionMind& MindOf(FactionId f) const { return minds_[f]; }
    // ...and for a test, to plant a belief or a stock. What is planted before the first
    // macro pass stands; the seeding only adds what is missing.
    FactionMind& MindOf(FactionId f) { return minds_[f]; }
    // Surveys, settlements and sieges under way, by id (#295).
    const std::map<int, Plan>& Plans() const { return plans_; }
    // The delivery contracts short markets have open, by id (#343, #407).
    const std::map<int, Contract>& Contracts() const { return contracts_; }
    // A holding keeps this share of its capacity at home; only what is above it can reach out
    // -- to move, to lend a siege, or to escort a convoy.
    static constexpr float HOME_GUARD = 0.6f;

    // --- Expeditions, growing stations and the builder convoy (#342 S6; Simulation_Growth.cpp) ---
    // What a blueprint costs a faction's stock, counted as one number; zero for none.
    static float BlueprintCost(const std::string& blueprint);
    // `f` sets an upgrade going (admin, a test, and the faction's own turn): its finished
    // outpost -- or outpost market -- in `systemId` into `blueprint`, paid now from its stock
    // and built from what a convoy brings. The plan's id, or 0 when it may not: not its own,
    // not what the blueprint is built onto, the region full of stations, or not the stock.
    int BeginUpgrade(FactionId f, const std::string& systemId, const std::string& blueprint);
    // ...and an Anchor at the unstable mouth into `slot`, the same way. 0 when it may not.
    int BeginAnchor(FactionId f, int slot);
    // How a convoy of `f` bound for `target` would go (#407): the way it fears least from its
    // holding with the most stations within the convoy rules' reach -- or from its nearest
    // holding, or the site itself -- the escort it would ask for and could have, the worst
    // danger on the way in ships, and how much of its freight it expects to lose, all from
    // what `f` believes. `pilots` is escort strength pilots already lend it where it sets out;
    // for each time it held one back or one turned back (`waits`) it asks for more escort.
    struct ConvoyChart
    {
        std::vector<std::string> route;
        float                    escort = 0.0f;  // what its holding can lend, of what it wants
        float                    worst = 0.0f;
        float                    loss = 0.0f;  // expected share of the freight, may pass 1
    };
    ConvoyChart ChartConvoy(FactionId f, const std::string& target, float pilots = 0.0f,
                            int waits = 0) const;
    // What happened to convoys since the world began (#407): set out, held back, and how each
    // that ended its way did. Transient: counted for a long run's report.
    struct ConvoyTally
    {
        long long sent = 0, held = 0;
        long long intact = 0, damaged = 0, turnedBack = 0, destroyed = 0;
        long long escorted = 0;  // of those that arrived, with a pilot with it at some point
    };
    const ConvoyTally& Convoys() const { return convoyTally_; }
    // ...and the same by the region of the site, by slot.
    const std::map<int, ConvoyTally>& ConvoysByRegion() const { return convoyRegions_; }
    // How many stations region `slot` has, and has going up.
    int StationsIn(int slot) const;
    // Whether a faction would want a shipyard for `systemId` (#432): none in its region,
    // standing or going up, and none within the growth rules' `shipyardReach` jumps.
    bool ShipyardWanted(const std::string& systemId) const;
    // How many jumps from `systemId` the nearest market is -- a station's, or an outpost
    // market's: 0 where it has one, -1 where none is within reach.
    int MarketJumps(const std::string& systemId);
    // What an outpost costs a faction's stock (#295): its blueprint's cost, counted as one
    // number. Zero when there is no outpost blueprint, which means no faction can settle.
    static float OutpostCost();
    // What mounting a siege costs (#295, slice 5): the siege rules' share of an outpost.
    static float SiegeCost();
    // The strength of `f` in a system that is besieging an outpost there rather than
    // patrolling it: warships where the system is hot, never the director's police or pirates.
    float SiegeForceIn(const std::string& systemId, FactionId f) const;
    // The strength `f` has lent a siege out of a holding and not yet had back (#295): what the
    // holding is short of while it is away, and does not regrow in the meantime.
    float LentFrom(const std::string& systemId, FactionId f) const;
    // Who would lend `f` a siege of `target`, nearest first: its holdings within the siege
    // rules' reach, each with what it can spare, until `want` is met. Ties go by system id.
    // `via`, if given, is the target's neighbour the nearest of them comes in through.
    std::vector<Plan::Lent> PoolFor(FactionId f, const std::string& target, float want,
                                    std::string* via = nullptr) const;
    // Where `f` would build an outpost for `purpose` in `systemId`, arriving from `from` build an
    // outpost for `purpose` in `systemId`, arriving from `from`
    // (#318): by the richest belt, between the gates, round a planet, by the wrecks, out of
    // sight, or at an open point -- always somewhere SpotProblem allows, and for a satellite
    // in a free orbit of its planet. Drawn from who, where and what for, so the same world
    // gives the same answer. Not found when the purpose has nothing to build by here or no
    // room left; the caller falls back to an open point.
    struct OutpostSpot
    {
        bool                 found = false;
        Vector2              at{ 0.0f, 0.0f };  // where it stands now
        std::optional<Orbit> orbit;             // set: a satellite, placed by the clock (#210)
    };
    OutpostSpot FindOutpostSpot(FactionId f, Outposts::Purpose purpose, const std::string& from,
                                const std::string& systemId) const;
    // The world's history, oldest first, capped (#295). Everything the news feed says is
    // here too, with a time and a sequence number; surveys are here and not in the feed.
    const std::vector<ChronicleEntry>& Chronicle() const { return chronicle_; }
    // Counts what a system's static layer holds (#295). Called when the layer is built
    // and whenever it changes; see SystemProfile.
    static SystemProfile ProfileOf(const SystemState& st);
    void                 StepFactions();
    void                 StepControl(bool settling);

    // --- Territory (#431 slice 5, Simulation_Territory.cpp) ---
    // What a structure weighs toward its owner's influence (WorldRules::Influence), and
    // whether it is a claim (`claim`): finished, of an outpost's tier or above. An outpost
    // being upgraded is still the outpost it was. 0 for a site, a buoy, a battery.
    static float InfluenceWeight(const Structure& t, bool& claim);
    // Every party's influence in `st` now (T1): its profile with owners read as their
    // corporations, and each faction's presence.
    InfluenceMap InfluenceIn(const SystemState& st) const;
    // Claims by corporations (T2), every macro pass, hot or cold: a corporation with a claim
    // and more influence than anyone else for ContestPasses() takes a system; a holder whose
    // last claim is gone, or whose corporation is, loses it at once; and a held system goes to
    // whichever party -- corporation or faction -- with a claim outweighs it as long. Called
    // by StepControl, whose faction rule leaves a system a corporation holds alone.
    void StepClaims(bool settling);
    // The systems a corporation holds, by id.
    std::vector<std::string> HeldBy(const std::string& corpId) const;
    // Who holds a system, as a party's key (InfluenceMap::Key, or a corporation's id): empty
    // for nobody's.
    static std::string HolderOf(const SystemAggregate& a);
    // The front line (#431 D3, slice 8): whether `side` -- a party's key, a faction's
    // InfluenceMap::Key or a corporation's id -- may besiege a structure in `system` or lay a
    // claim to it. True where nothing is to be crossed (the system is nobody's or the side's
    // own); otherwise only from next door: a gate neighbour the side holds, an ally of a
    // faction side holds, or nobody holds. A neighbour is a link in the route graph -- gates,
    // admin links and a mouth's gates while it stands -- never a roaming hole, which is a
    // pilot's secret and closes. It reads the aggregates alone, so a cold system is the same
    // as a hot one. It binds sieges and claims, never a ship's guns (Decided after the
    // rounds, 3).
    bool FrontOpen(const std::string& system, const std::string& side) const;
    bool FrontOpen(const std::string& system, FactionId f) const
    {
        return FrontOpen(system, InfluenceMap::Key(f));
    }
    // The share of a trade a market in `st` takes (T3): its holder corporation's tax, or the
    // standard fee (Goods::Rules().fee) where no corporation holds.
    double FeeRateIn(const SystemState& st) const;
    // Whose a trade's fee is at `market` in `st` (T3, #438): the holder corporation's where one
    // holds; otherwise the market's owner -- a faction's treasury, or for a player's market
    // the owner's corporation's, or the pilot's own when in none.
    struct FeeTo
    {
        double      rate = 0.0;
        bool        toFaction = false;
        FactionId   faction = Factions::Unaffiliated();
        std::string corp;     // a corporation's treasury
        std::string account;  // a pilot in none
    };
    FeeTo FeeAt(const SystemState& st, const Entity& market) const;
    void  PayFee(const FeeTo& to, double fee);

    // --- Licences (#431 slice 6, Simulation_Territory.cpp) ---
    // Building in a system a faction or a corporation holds takes the holder's leave and a
    // licence, a weekly rent (WorldRules::Licences). A licence is its party's: a corporation's,
    // on its record and paid from its treasury, or a pilot's in none, in the world's book of
    // owners and paid from their money. Rent comes due in the coarse pass, hot or cold, and is
    // taken if the party can pay and the holder still admits it; unpaid a grace later, it
    // lapses, and what the party has there trespasses: lawless to shoot (LawOver), and the
    // holding faction may weigh evicting it (Think).
    //
    // Who grants a licence in `st`: a holder corporation's id, or a lawful faction's key
    // (InfluenceMap::Key); empty where none is needed -- nobody's system, the frontier, the
    // lawless', or what the world's unaffiliated keep.
    std::string LicenceHolder(const SystemState& st) const;
    // ...as a player reads it: the faction's name, or the corporation's name and ticker.
    std::string HolderName(const std::string& holder) const;
    // The party `account` builds for: their corporation's id, or InfluenceMap::PILOT + account.
    std::string PartyOf(const std::string& account) const;
    // A party's licence in a system; null for none.
    const Corps::Licence* LicenceOf(const std::string& party, const std::string& system) const;
    // A party's standing with a faction: a pilot's own, a corporation's the sum of its members'
    // (Decided after the rounds, 2) -- each as last known, 0 for one never seen.
    float PartyStanding(const std::string& party, FactionId f) const;
    // Whether `holder` would license `party` in `st`: a faction for a standing of the rules'
    // or more, a corporation by its docking policy. `why` says why not.
    bool Admits(const SystemState& st, const std::string& holder, const std::string& party,
                std::string& why) const;
    // Why `account` may not build in `st` now for want of leave or a licence; empty: free to.
    std::string LicenceProblem(const std::string& account, const SystemState& st) const;
    // The verb (Command::buyLicence): a period's rent, paid now, for the system the pilot is
    // in. A licence still paid for is extended a period from where it runs to.
    bool BuyLicence(ClientSession& s);
    // What building in the pilot's system takes, for their snapshot; a corporation's licences
    // for its members' view.
    Proto::LicenceView              LicenceViewFor(const ClientSession& s) const;
    std::vector<Proto::LicenceView> CorpLicences(const std::string& corpId) const;
    // Once a pass: rent due is taken or goes unpaid, unpaid becomes lapsed after the grace, a
    // licence for a system the party has nothing in ends with its period, and every player
    // structure in a held system is told where it stands (Structure::SetLicence).
    void StepLicences();
    // ...the last part for one system, at once: after a purchase, a build, a change of holder.
    void RefreshLicences(SystemState& st);
    // A period's licence for `account`'s party in `system`, without the purchase or its gate:
    // what a test that builds in a held system starts from. Nothing where none is needed.
    void        GrantLicence(const std::string& account, const std::string& system);
    static void SeedPresence(SystemAggregate& a);
    // Where a system's security settles (#428): its base, moved by the police/pirate balance.
    static float SecurityTarget(const SystemAggregate& a);
    // At most this far from the base either way, and the softness of the balance: with
    // police and pirates at zero it is even, and a lone ship of either moves it a fifth.
    static constexpr float SECURITY_SWING = 0.15f;
    static constexpr float SECURITY_BALANCE_SOFTNESS = 4.0f;
    static constexpr int   ContestPasses() { return CONTEST_PASSES; }

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

    // Fixed defences (#193): anything `defensive` -- a station, or a finished structure whose
    // archetype is armed (#39) -- fires at the nearest hostile ship within its radius plus
    // its range, once a second, for its archetype's damage per second. Hostile means what it
    // means to an NPC of the owning faction: a player by their own account, an NPC by the
    // relation matrix. A structure also fires back at any player who has shot at it lately,
    // which is all a player's own has to go on -- and at anyone who has shot its owner's
    // ship within its reach (ProvokeGuards). A hidden or docked player is not in `players`,
    // so is not shot at.
    void StepStationDefence(SystemState& st, const std::vector<PlayerPresence>& players,
                            std::vector<FireEvent>* fires, float dt);
    // A player's structure fired first, as its rules of engagement told it to (#340): the
    // crime that is, if any, is its owner's -- against a lawful faction's ship, or against
    // a player the system's lawful holder protects. Per hit, and the kill on top.
    void ChargeFirstStrike(SystemState& st, const Entity& structure, const ClientSession* player,
                           const NpcShip* npc, bool killed);

    // --- Law (#431 slice 9; Simulation_Agents.cpp, numbers in WorldRules::Law) ---
    // Whether `by` attacking `victim` in `st` is a crime, and under whose law (#41): a lawful
    // faction's ship is that faction's wherever it is; a structure is under LawOver; another
    // pilot's ship is under the law of the system it is in (#431 slice 10): a crime where a
    // lawful faction holds it at a security above zero, unless the pilot is fair game -- flagged
    // a criminal, or an enemy of that law (AccountHostileToFaction). The one place that
    // decides it.
    bool IsCrime(const SystemState& st, const ClientSession& by, const Entity& victim,
                 FactionId& law) const;
    // The pilot flying the ship with entity id `shipId`, if any.
    const ClientSession* PilotOf(int shipId) const;
    // Another pilot's ship that `by` can aim at in `st` (#431 slice 10): in that system, in
    // space, not in cover and not already lost. Null otherwise. A lingering ship is a target:
    // that is what lingering is for (#341).
    ClientSession* PilotTarget(const ClientSession& by, const SystemState& st, int shipId);
    // What a crime costs the pilot on top of standing and bounty (#431 slice 10): the
    // criminal flag, renewed, and a fall in security status -- more for a kill.
    void MarkCrime(ClientSession& s, bool killed);
    // The pilot's own law clock, every world tick while they are in the world (not while
    // their ship lingers after a disconnect): the flag runs down, and without one the
    // security status recovers towards zero. Says so when the flag lapses.
    void StepPilotLaw(ClientSession& s, float dt);
    // Whether `police` engages `p` on sight for their security status alone: police of a
    // lawful faction, in a system of WorldRules::Law::onSightFrom or more, and a pilot at
    // -onSightBelow or below.
    bool PoliceOnSight(const SystemState& st, const ClientSession& p, const NpcShip& police) const;
    // What an NPC holds against a player in `st`: its faction's enmity by account, and the
    // police's on-sight rule.
    bool NpcHunts(const SystemState& st, const ClientSession& p, const NpcShip& n) const;
    // A crime under `law` by `by` in `st`: the police answer from real forces, as the system's
    // security band says (WorldRules::Law) -- every free police ship here, patrols a station
    // launches out of what the faction can spare, and police lent by systems within reach;
    // quicker and heavier the higher the security, nobody at all below the lowest band. Only
    // in a hot system, once per call. True when anyone is coming.
    bool CallPolice(SystemState& st, ClientSession& by, FactionId law);
    // A call whose answer from here was delayed, answered when its time comes. Every tick.
    void StepPoliceCall(SystemState& st);
    // Whether a station's or a faction structure's guns, owned by `owner`, fire on `p` without
    // being shot at first: a pilot its faction holds as an enemy anywhere, and a criminal it
    // wants only where security brings the guns in (WorldRules::Law::gunsFrom).
    bool GunsOn(const SystemState& st, const ClientSession& p, FactionId owner) const;

    // One full step of a system: AI, movement, combat, cleanup of the fallen. `players`
    // is whoever happens to be standing in it. A cold system (#295) has no ships to step
    // and is kept by the coarse pass instead: this does nothing for one.
    void StepSystemAgents(SystemState& st, const std::vector<PlayerPresence>& players,
                          std::vector<FireEvent>* fires, float dt);

    // A player's ship was destroyed (#344). The ship being flown is lost -- its hull leaves
    // the hangar and its fit goes with it; the starter is free and unlimited, so losing it
    // loses only the fit -- and so is the cargo. A wreck holding a share of what was lost lies
    // where it died. Insurance on the hull pays (InsurancePayout) and is spent. The pilot
    // wakes docked at their home (HomeOf) in the starter; with nowhere that takes them, beside
    // a station of the system they died in that would not shoot them (#224), undocked. Says
    // all of it in the journal and in ClientSession::lastLoss.
    void ServerRespawnPlayer(ClientSession& s);
    // Where a pilot wakes (#344): their home station if it is still there and its owner
    // would let them dock, else the trade hub of the first start that would; 0 -- nowhere.
    // `systemId` is set to the station's system.
    int HomeOf(const ClientSession& s, std::string& systemId) const;
    // What hull `catalogIndex` of this account is worth as it is fitted (ShipValue), at the
    // ore prices of `st`'s market.
    double ValueOf(const ClientSession& s, int catalogIndex, const SystemState& st) const;
    // How long the wreck of a player's ship lies where it fell, in world seconds (#344).
    static constexpr float SHIP_WRECK_SECONDS = 3600.0f;

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

    // Creates an NPC with a stable id and puts it into the system. Which of its doctrine's
    // designs it flies (#279) is picked by `pick`: a system warming from its aggregate passes
    // the ship's place in its role, so the n-th patrol of a system flies the same design every
    // time it warms, whatever else happened first; -1 (a ship sent in later) picks by its id.
    void SpawnNpcInto(SystemState& st, Vector2 pos, FactionId faction, NpcRole role,
                      std::vector<Vector2> waypoints, int pick = -1);
    // Recounts live NPCs by role into the system aggregate. A cold system has none to count:
    // its aggregate is the truth, and this leaves it alone.
    void RecountAgg(SystemState& st);
    // The pirates a hot system has right now, counted without touching the aggregate.
    static int LivePirates(const SystemState& st);
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
    // Loads the static objects of all systems from systemsDir (with a trailing '/'): the
    // stations, bodies, gates and structures, and nothing that flies. Ships are made only
    // where a player is (Warm), unless every system is kept hot (SetAllHot).
    void MaterializeAllSystems(const std::string& systemsDir);

    // --- Cold systems (#295) ---
    // A system is hot -- its NPC ships real and stepped every tick -- while a player is in it
    // and for COOL_AFTER seconds after the last one has gone; otherwise it is cold, and its
    // population, its losses, its market and its bodies are kept by arithmetic once a coarse
    // pass. Production, presence, the faction step and the macro drift read the aggregates
    // either way, so the world does not stop where nobody is; it only stops being drawn.
    //
    // A minute: a player who jumps out and straight back -- running from a fight, or looking
    // through a gate -- finds the same ships where they were, and so does one whose
    // connection dropped and came back. Not much longer, so that somebody flying a route
    // leaves at most the last system or two of it hot behind them: what an empty server
    // costs is set by where its players are, not by how big the map is.
    static constexpr float COOL_AFTER = 60.0f;
    static bool            IsHot(const SystemState& st) { return st.populated; }
    // Makes a system hot now if it is not -- its ships made from its aggregate, among its
    // bodies where the clock puts them -- and keeps it hot for COOL_AFTER from now. Every
    // tick a player is in a system does this for it (MaintainWorld), and so does arriving.
    void Warm(SystemState& st);
    // Every system hot all the time, as before cold systems existed: for comparing the two
    // (`econserver <ticks> --hot`, `macrobench --hot`) and for tests about ships in a system
    // nobody is in. Set before MaterializeAllSystems.
    void SetAllHot(bool all) { allHot_ = all; }
    bool AllHot() const { return allHot_; }

    // --- Ships that leave (#295; Simulation_Migration.cpp) ---
    // A system holding more ships of a role than it keeps sends one at a time to a neighbour
    // that lacks them, at most one per role per the interval factions.json gives. Where the
    // system is cold that is a number taken off it; where it is hot, a real ship turns for the
    // gate and is gone. Either way it is on its way for `travel` seconds, counted nowhere, and
    // then counts where it arrives -- as a ship coming in through the gate if somebody is there.
    struct Migrant
    {
        long long   id = 0;
        std::string from, to;
        NpcRole     role = NpcRole::Trader;
        FactionId   faction = Factions::Unaffiliated();
        double      leftAt = 0.0, arrivesAt = 0.0;
        // While it is still turning for the gate in a hot system: which ship it is, and when it
        // goes. Transient -- a restart begins cold, and every migrant has gone by then.
        int    shipId = 0;
        double jumpAt = 0.0;
        // What a hauler carries (#343): bought from the market it left, sold to the one it
        // arrives at, and in neither while it is on its way. Saved.
        ResourceType good;
        int          cargo = 0;
    };
    const std::map<long long, Migrant>& Migrants() const { return migrants_; }
    // Goods on their way between systems, by good: what haulers carry and no market holds.
    std::vector<double> GoodsInTransit() const;
    // How many ships of `role` the director keeps in a system, before the pressure of recent
    // losses: what it holds above this is surplus, and leaves.
    int KeepOf(const std::string& systemId, NpcRole role) const;

    // M4: builds the WORLD snapshot of a system (entities with id/kind/position, etc.) for
    // the client. Player state and fire events are added by the caller (the client owns the
    // player ship until M4f).
    Proto::Snapshot BuildSnapshot(const ClientSession& s, const std::string& systemId) const;
    // The ids a player's snapshot carries in full wherever they are (#349): what is selected,
    // what the ship holds station on, the object of its standing order and the station it is
    // docked at. Everything else is in full only near the ship (Proto::Interest).
    std::set<int> InterestOf(const ClientSession& s) const;

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
    // `fromHere` false leaves out the reach: whether it could be laid down once the ship is
    // there, which is what a build order asks before it flies.
    std::string PlacementProblem(const ClientSession& s, const Blueprint& bp, Vector2 at,
                                 bool fromHere = true) const;
    // Lays down a construction site of `blueprint` at `at`, named `name` (empty -- the
    // blueprint's name), owned by the session's account. Takes the cost from the hold and
    // answers in the journal either way. Returns the site's id, or 0 when refused.
    int Deploy(ClientSession& s, const std::string& blueprint, Vector2 at, const std::string& name);
    // An upgrade blueprint (one with `onto`) is laid down onto the player's own outpost at `at`
    // (#433): Deploy's work once PlacementProblem has said yes. The outpost becomes the site,
    // keeps its id and its owner, and is named `name` if one is given. Returns its id.
    int Upgrade(ClientSession& s, const Blueprint& bp, Vector2 at, const std::string& name);
    // Why `bp` may not stand at `at` in this system, or empty when it may: inside the
    // system, clear of bodies and the paths they sweep, clear of what others use, and
    // under the blueprint's cap per system. The part of PlacementProblem that is about the
    // place rather than the builder; a faction's outpost is placed by it too (#295).
    std::string SpotProblem(const SystemState& st, const Blueprint& bp, Vector2 at) const;
    // The structure an upgrade (#433) laid down at `at` is built onto: the one whose disc
    // `at` falls on, the nearest of them; null when there is none. Whether it may be is
    // PlacementProblem's to say.
    static const Structure* UpgradeTarget(const SystemState& st, Vector2 at);
    // Whether docking at `e` is refused this pilot, and why (#433): a faction's dock for a
    // pilot it hates or a lawful faction's for a flagged criminal (#431 slice 10), a player's
    // structure without a market for anyone but its owner and the owner's corporation (#347),
    // and in a system a corporation holds, its structures for whom its docking policy leaves
    // out (#431 T3). Empty: admitted.
    std::string DockRefusal(const ClientSession& s, const Entity& e) const;
    // Finishes every site whose time has come and takes away every structure whose time is
    // up. Each is an instant fixed when the site went down -- nothing is integrated -- so a
    // restart in between changes nothing. Called by MaintainWorld every tick, and looks at
    // nothing that is not due (#295): the instants wait in a queue, filled when a structure
    // is added and when the static layers are built.
    void StepStructures();
    // How many structures this account has standing, in the whole galaxy.
    int StructuresOwnedBy(const std::string& account) const;

    // --- Taking things down (#39, slice 2) ---
    // Why this player may not dismantle structure `id` now, or empty when they may: it is
    // theirs, in their system, within its blueprint's reach of the ship, the ship is in
    // space and not warping, and the hold has room for what comes back.
    std::string DismantleProblem(const ClientSession& s, int id) const;
    // The owner takes their own site or structure apart: the refund the blueprint's rules
    // give (Blueprints::Refund) goes into the hold and the object leaves the world through
    // RemoveStatic. Answers in the journal either way; false when refused.
    bool Dismantle(ClientSession& s, int id);

    // --- Rules of engagement (#340) ---
    // Why this player may not set the rules of structure `id` to `d`, or empty when they may:
    // it is theirs -- or a member's of the corporation they are CEO or a director of (#347) --
    // in their system, armed (or will be once built), and the rules are sound
    // (DefenceProblem). From anywhere in the system: a setting, not a thing done to it.
    std::string DefenceSetProblem(const ClientSession& s, int id, const Defence& d) const;
    // Sets them, tells everyone in the system by a delta, keeps them in the save, and says
    // in the owner's journal what they now cost there. False, with a Notice, when refused.
    bool SetDefence(ClientSession& s, int id, const Defence& d);
    // The owner's standing with `f` as far as the world knows it: live while the owner is
    // here, else as of when they last were (OwnerBook). False when it has never been seen.
    bool OwnerStanding(const std::string& account, FactionId f, float& out) const;
    // A crime an owner's structure committed (#41's table, ChargeAttack): charged at once
    // while the owner is connected, and on their return otherwise.
    void ChargeOwner(const std::string& account, FactionId victim, bool killed);
    // Someone shot `victim`'s ship at `at`: every armed structure of theirs -- or of anyone in
    // their corporation (#347) -- in reach of that point counts it as a shot at itself, and
    // fires back for as long (#340). The shooter is a
    // player (`bySession`, for the structure they fired with) or an NPC (`byNpc`), 0 when not.
    void ProvokeGuards(SystemState& st, const ClientSession& victim, Vector2 at, int bySession,
                       int byNpc);
    // Owed charges settled and standings noted, for every connected owner. Every tick.
    void StepOwners();

    // --- Attacks told to owners (#431 C2, slice 13) ---
    // An attack on a player's structure goes into the journal: the owner's own, or their
    // corporation's (Corps::Corp::journal) when they are in one. Hits closer together than
    // ATTACK_EPISODE_SECONDS are one attack, told once when it is over (or the structure is
    // gone) -- a siege of hours is one entry -- and, to whoever is on, once when it starts.
    // Nobody on to read it: the owner's entry waits in their account save (the pending part
    // of the journal) and their corporation's in corps.json, and each is in the journal at
    // login. A Discord relay (a later slice) would read the same entries.
    static constexpr double ATTACK_EPISODE_SECONDS = 600.0;
    // The pending part of an account's journal, kept in its save, at most this long.
    static constexpr size_t MAX_PENDING_JOURNAL = 20;
    // Journal entries for accounts that were not on, for the host to append to their saves
    // (AppendPendingJournal). Taken, so each is written once; capped while nobody takes them.
    std::vector<std::pair<std::string, Ev::Event>> TakeAbsentEvents();
    // Appends `events` to the pending journal of the account saved at `path`, keeping the
    // last MAX_PENDING_JOURNAL. Missing, damaged or newer files are left as they are (#20).
    static Save::Result AppendPendingJournal(const std::string&            path,
                                             const std::vector<Ev::Event>& events);
    // Attacks that have gone quiet, ended and told. Every tick, from StepOwners.
    void StepAttacks();
    // What the world remembers about an attack under way on structure `key`
    // ("<system>/<static key>"): whose it is, and what has happened so far.
    struct AttackEpisode
    {
        std::string owner, name, system, by;  // `by`: who attacked, as last seen
        int         hits = 0;
        float       hull = 1.0f;  // what was left after the last hit
        double      first = 0.0, last = 0.0;
    };
    const std::map<std::string, AttackEpisode>& Attacks() const { return attacks_; }
    // How much of a crime is waiting for its owner: what ChargeOwner put by, and not yet
    // settled. Zero for an account that owes nothing.
    float OwedStanding(const std::string& account, FactionId f) const;
    // The faction whose law makes attacking this structure a crime, if any (#41): a lawful
    // faction's own outpost is that faction's; anything else -- a player's, the world's --
    // is under the law of the system's holder when that is a lawful faction. Nobody's in a
    // system nobody holds, a pirate's, and never for its own owner.
    bool LawOver(const SystemState& st, const Structure& t, const std::string& attacker,
                 FactionId& law) const;
    // Damage to a structure, from `by` (null -- not a player). Every hit is a change to the
    // layer (MarkStaticChanged) so everyone sees its hull, and a save keeps it. At no hull
    // left it is destroyed: its wreck, if its blueprint names one, goes down where it stood
    // holding the rules' share of the cost, and the structure goes through RemoveStatic.
    // Returns true when this hit destroyed it.
    bool DamageStructure(SystemState& st, int id, float damage, ClientSession* by);
    // How a structure was brought down, for what the history says of a faction's outpost: by
    // somebody's guns, by a faction's siege, or by nobody supplying it.
    enum class Loss
    {
        Attack,
        Siege,
        Decay
    };

    // M4e-3c: galaxy snapshot (statistics + news) for the networked client's galaxy map.
    // Reads the world without changing it (#341). GalaxyStats has every system and is the
    // server's own; a pilot is sent GalaxyStateFor -- only the systems on their charts, for
    // nobody knows how a system they have not seen is doing (#144, #431).
    Proto::GalaxyState GalaxyStats() const;
    Proto::GalaxyState GalaxyStateFor(const ClientSession& s, const Proto::GalaxyState& all) const;
    Proto::GalaxyState BuildGalaxyState(const ClientSession& s) const
    {
        return GalaxyStateFor(s, GalaxyStats());
    }

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

    // A ship whose player dropped the connection stays in space this long (#341). Leaving
    // used to take the ship out of the fight at once, so pulling the cable was the best way
    // out of a lost one. A docked ship goes at once: the station is a refuge either way.
    static constexpr float LINGER_SECONDS = 30.0f;
    // Starts the countdown for a session whose connection is gone. False if there is nothing
    // to leave behind -- docked, or no ship -- and the caller ends the session now.
    bool BeginLinger(ClientSession& s);
    // Counts every lingering session down; returns those whose time is up, for the host to
    // save and end. The session lives on until then: NPCs see it and shoot at it, and other
    // players see it too.
    std::vector<int> StepLinger(float dt);

    // Whether this player is inside something that hides ships -- a nebula today; the test
    // asks for the property rather than the class (#34), so a jammer a player builds would
    // hide them too. Hidden from NPCs and, since #341, from other players' snapshots.
    bool InCover(const ClientSession& s) const;

    // Commands that exist only for development (F1's money). Off unless the server was started
    // with --dev (#341): they used to be honoured for anyone who could open a socket.
    void SetDevCommands(bool on) { devCommands_ = on; }
    bool DevCommands() const { return devCommands_; }
    // F1: credits to the account, if dev commands are on. False, and nothing given, if not.
    bool GrantDebugMoney(ClientSession& s);

    // Server step of a player ship: applies the movement axes/toggles from the command,
    // the piloting bonus, and updates the physics. Combat/mining — via separate methods.
    void StepPlayerShip(ClientSession& s, const Proto::Command& cmd, float pilotBonus, float dt);
    // How far a player's command moves the ship (#432): a whole tick, unless a standing order
    // is flying it. The order steps the ship once every world tick, so a command then carries
    // only its intents -- a human client streams one a tick through an order, and each used to
    // step the ship again: a warp ran at twice its rate on the server and was pulled back on
    // the client at every snapshot. One step a tick is what the client predicts either way.
    static float CommandDt(const ClientSession& s, float dt)
    {
        return s.HasRunningOrder() ? 0.0f : dt;
    }

    // Once per world tick, after the world has moved: what the player's ship is attached
    // to (#298). A docked ship is carried to its berth beside the station, wherever the
    // station has gone on its orbit (#210), and a ship holding station on something has
    // that something's velocity measured, for a follow to match. Commands arrive in bursts
    // and an agent sends almost none, so neither can be left to the command path.
    void StepPlayerAttachment(ClientSession& s, float dt);

    // Player weapon range lives in sim/PlayerStep.h — a single source shared with the
    // client, which draws the targeting circle from it.
    static constexpr float PLAYER_WEAPON_RANGE = Sim::PLAYER_WEAPON_RANGE;
    // How long an armed structure keeps firing back at a player who shot at it (#39).
    static constexpr float PROVOKED_SECONDS = 60.0f;

    // Facts of the player's combat: the server applies damage and credits reputation, bounty
    // and mission progress to the session's account itself; these are reported so the client
    // can draw the consequences from facts rather than recompute them.
    struct PlayerCombatEvents
    {
        bool      hitLawful = false;  // hit a lawful target (a crime)
        FactionId hitFaction = Factions::Unaffiliated();
        bool      killedPirate = false;  // killed a pirate (mission credit)
        bool      killedLawful = false;  // killed a lawful target (a serious crime)
        FactionId killedFaction = Factions::Unaffiliated();
        Vector2   shotFrom = { 0.0f, 0.0f };  // player's shot beam (for render/snapshot)
        Vector2   shotTo = { 0.0f, 0.0f };
        int       victimSession = 0;  // another pilot was hit: their session (#431 slice 10)
        bool      killedPilot = false;
    };
    // Server player combat: if the weapon is on and the target (targetId) is in range and
    // the cooldown is ready — applies damage, accumulates facts in ev. Returns true if there
    // was a shot this tick (the client draws a beam). The target is looked up in st by id.
    // A path across the whole gate graph, as system ids from `from` to `to` inclusive --
    // what the world allows, charted or not, with no mass. Empty when there is no route, or
    // when either end is unknown. A pilot's route order plans over what that pilot knows
    // instead (RouteFor); both are Route::Find (#432).
    //
    // `preference` weighs each hop by the security of the system entered: the short way,
    // the high-security way or the low-security way. `extra` are links beyond the gates
    // that this traveller may take: the roaming holes a pilot has found (#342 S9).
    std::vector<std::string>
    PlanRoute(const std::string& from, const std::string& to,
              Route::Preference                           preference = Route::Preference::Shortest,
              const std::vector<WorldLoader::SystemLink>* extra = nullptr) const;
    // The route a pilot's order flies from where the ship is (#432): over the systems that
    // pilot knows (KnownUniverseFor) -- charted, the frontier one gate past them, and the
    // holes they found -- through the order's waypoints, under its preference, with the
    // ship's mass against every mouth and hole. The same search the client's map previews.
    Route::Result RouteFor(const ClientSession& s) const;

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
    // ...at another pilot's ship (#431 slice 10): in range and off cooldown, the hit goes on
    // the victim's ship, IsCrime decides what it costs the shooter, and a kill is the loss
    // every ship takes (ServerRespawnPlayer, on the world tick).
    bool FirePilot(ClientSession& s, SystemState& st, ClientSession& victim,
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
        double fee = 0.0;      // the trade's fee, taken from the revenue (#348)
    };
    PlayerSellResult StepPlayerSell(ClientSession& s, SystemState& st, int resourceType,
                                    int amount);
    // Buy from the system market at the station docked at (#343): as much as the market has,
    // the hold takes and the money pays for, up to `amount`, at the ask -- each unit dearer
    // than the last -- with the fee on top. A purchase that comes to nothing says why in the
    // journal.
    struct PlayerBuyResult
    {
        int    bought = 0;
        double cost = 0.0;  // what the account paid, the fee included
        double fee = 0.0;
    };
    PlayerBuyResult StepPlayerBuy(ClientSession& s, SystemState& st, int resourceType, int amount);

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
    // Insure an owned hull, as it is fitted now, docked at any station (#344): its value at
    // this station's ore prices is covered, for InsurancePremium. Refused for a hull already
    // covered for what it is worth, or with too little money. true -- insured.
    bool InsureShip(ClientSession& s, int catalogIndex);
    // Refit the ship being flown to `fit` (#279 fitting): only docked at a station with a
    // shipyard, only the fit of the hull's own sections (Ships::PlayerFit), paid in credits
    // at the yard's price (Ships::RefitPrice, by standing as a ship is), and refused if the
    // hold would no longer take the cargo. The design is filed and the ship flies it from
    // now on. true -- refitted.
    bool RefitShip(ClientSession& s, const std::vector<Ships::FitLine>& fit);
    // The named design library (#279 fitting, step 3). Each answers in the journal, on
    // success and on refusal, and returns whether it did anything.
    // - SaveDesign files `fit` on the hull being flown under `name`, anywhere: saving costs
    //   nothing and changes no ship. A name already in the library is saved over; a new one
    //   is refused once the library holds Ships::LIBRARY_MAX.
    // - ApplyDesign refits the ship being flown to a saved design of its own hull: exactly
    //   RefitShip with that design's fit, so it is priced and refused by the same rules.
    // - RenameDesign and DeleteDesign change the library and nothing else; a design a ship
    //   still flies stays filed for that ship.
    bool SaveDesign(ClientSession& s, const std::string& name,
                    const std::vector<Ships::FitLine>& fit);
    bool ApplyDesign(ClientSession& s, const std::string& name);
    bool RenameDesign(ClientSession& s, const std::string& from, const std::string& to);
    bool DeleteDesign(ClientSession& s, const std::string& name);
    // A design was filed since the last call: the host writes designs.json now rather than
    // at the next checkpoint, so a crash cannot leave a paid-for ship naming nothing.
    bool TakeDesignsChanged()
    {
        const bool c = designsChanged_;
        designsChanged_ = false;
        return c;
    }
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

    // --- Jobs a faction's plans post (#345; Simulation_Jobs.cpp) ---
    // The jobs on the board of station `stationId` in the session's system: what the plans of
    // its faction within the jobs rules' reach need done, nearest first, at most a board's
    // worth, priced by the session's standing -- and only what the faction can pay for and the
    // session does not hold already. GenerateDockOffers puts them before the station's own work.
    std::vector<Mission> JobsPostedAt(const ClientSession& s, int stationId) const;
    // Whether a plan's job can be handed in now: anywhere for one that is done, and for a
    // supply job with the goods aboard in the site's system -- beside the site in flight, or
    // docked at any station there (#429).
    bool PlanJobReadyNow(const ClientSession& s, const Mission& m) const;
    // A supply job's site, if it is in the session's system: its entity id, else 0.
    int JobSiteId(const ClientSession& s, const Mission& m) const;
    // An escort job's convoy as it is now (#430), into its view: where it is and goes next,
    // and its hauler and escorts when they are in the session's system.
    void ConvoyView(const ClientSession& s, const Mission& m, Proto::MissionView& v) const;
    // A supply job's site by name, wherever it is; empty when it is not a supply job's or the
    // site is gone (#429).
    std::string JobSiteName(const Mission& m) const;
    // Once a pass: escorts that are with their convoy, convoys arrived or lost, sites that no
    // longer need anything, supply jobs delivered where they are ready (#429), and jobs out of
    // time.
    void StepJobs();
    // The session shot `killed` down: a besieger counts towards its break-the-siege jobs, and a
    // defender of a besieged outpost towards its join-the-siege jobs (#407).
    void CreditJobKill(ClientSession& s, const NpcShip& killed);
    // The besieged side's strength at a siege that a pilot can find there as ships -- its
    // pirates, or the police flying its flag -- in ships; 0 when it is numbers only (#407).
    float DefendersShown(const Plan& p) const;
    // A plan's site still going up, that a delivery helps: a settlement's, or an Anchor's or
    // an upgrade's come to its site. Null for anything else.
    const Structure* JobSite(const Plan& p) const;

    // --- Delivery contracts a short market posts (#343, #407; Simulation_Contracts.cpp) ---
    // Once a pass: pilots' contracts out of time, contracts ended (their price back in the
    // treasury), and once every contracts rules' `every` seconds, PostContracts.
    void StepContracts();
    // Every market short of a good posts a contract for it, if a market near sells it cheaper
    // than the contract pays and its owner's treasury holds the price.
    void PostContracts();
    // What a contract for `amount` of `good` at `st`'s market pays, `shortness` (0..1) below
    // the line: 0 when no market within reach sells the units for enough less.
    double ContractPrice(const SystemState& st, ResourceType good, int amount,
                         double shortness) const;
    // A system's market and its owner: the controller's station that trades, else the first
    // faction station that does. A stationId of 0 -- none.
    struct MarketHolder
    {
        int       stationId = 0;
        FactionId faction = Factions::Unaffiliated();
    };
    MarketHolder MarketStation(const SystemState& st) const;
    // The station a contract is for, if it still stands.
    const Entity* ContractStation(const Contract& c) const;
    // Whether a contract can be handed in now: docked at its station with the goods aboard.
    bool ContractReadyNow(const ClientSession& s, const Mission& m) const;

    // Server-side active-system change (for the headless host on a jump): makes destId
    // active and teleports the player to the gate leading back to fromId (as the arrival
    // point). A cold destination warms (#295): its ships are made before the player sees it.
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
    //
    // Written atomically (#341): beside the file, then renamed over it, so a crash leaves the
    // old world or the new one and never half of one. The replaced file is kept as
    // `.bak1`..`.bakN`. False if the write failed; the file is then as it was.
    static constexpr int WORLD_BACKUPS = 3;
    bool                 SaveWorld(const std::string& path) const;
    // Save::Result::TooNew means the file was written by a later build: it is left alone
    // and nothing is loaded, because reading it with this build's rules would turn an
    // unknown field into a default and then write that back (#20).
    Save::Result LoadWorld(const std::string& path);

    // Account persistence, one file per account name (the world has its own world.json).
    // Money, skills, reputation and wanted levels; where the player was and what they were
    // carrying (#49); which ships they own and which one they are flying (#5). Without it
    // a session would begin from nothing every time somebody connected.
    // Atomic like the world, keeping one older copy. Not const: the designs the account's
    // ships fly are noted as its own (DesignStore).
    static constexpr int ACCOUNT_BACKUPS = 1;
    bool                 SaveAccount(const ClientSession& s, const std::string& path);
    Save::Result         LoadAccount(ClientSession& s, const std::string& path);

    // Which world this is (#392): made once, when a world is first saved, and kept in
    // world.json. An account is stamped with it, so a pilot from another world -- one whose
    // world.json was moved aside while the accounts stayed -- keeps what they own and earned
    // but not where they were: system ids are alike from seed to seed, and the place would
    // be a different system under the same name. Empty for a world that is never saved (a
    // test), and then no account is anybody else's. An account with no stamp is from before
    // this, and belongs to the world that loads it.
    const std::string& WorldId() const { return worldId_; }
    void               SetWorldId(const std::string& id) { worldId_ = id; }
    // Whether an account stamped `accountWorld` was last saved in another world than this.
    bool ForeignWorld(const std::string& accountWorld) const
    {
        return !worldId_.empty() && !accountWorld.empty() && accountWorld != worldId_;
    }

    // The players' corporations (#347), kept beside the world in corps.json. The host applies
    // the verbs and writes the file when Rev() moves; the simulation reads membership, for
    // whom a structure counts as its owner's friend and who may manage it.
    Corps::Registry&       Corporations() { return corps_; }
    const Corps::Registry& Corporations() const { return corps_; }
    // Who may read whose allow-lists follows the corporations (Proto::ForViewer), so when they
    // change, every structure that has one is described again as a change: each viewer's
    // delta is cut afresh, and a new director sees the lists a member keeps.
    void RetellAllowLists();

    // The designs players' ships fly (#279 fitting), kept beside the world in designs.json.
    // SaveDesigns drops first what no save names and no live ship flies, so a design lives
    // exactly as long as a ship that might fly it. Load it before any account.
    DesignStore&       Designs() { return designs_; }
    const DesignStore& Designs() const { return designs_; }
    void               SaveDesigns(const std::string& path);
    Save::Result       LoadDesigns(const std::string& path);
    // What a ship of this design is under a player (PlayerStats), for a design filed here
    // or a stock one. False for an id that is neither.
    bool ShipStatsFor(const std::string& design, ShipStats& out) const;
    // Flies an owned hull as `design`: the session's record and, if it is the hull being
    // flown, the ship itself. The design must already be filed (or stock).
    void FlyDesign(ClientSession& s, int hull, const std::string& design);
    // The definitions a client has to be sent before this snapshot, because it names them
    // and they are not stock: every player design the snapshot names that is not in `sent`,
    // which it is added to (#279 fitting). The host keeps `sent` per connection, and sends
    // these first -- the same promise LayoutDelta keeps (#38).
    std::vector<Proto::DesignDef> DesignDefsFor(const Proto::Snapshot& snap,
                                                std::set<std::string>& sent) const;

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
    Corps::Registry corps_;
    DesignStore     designs_;
    bool            designsChanged_ = false;

    // What the spawn director keeps in a system (#295): how many ships of each role, after
    // the pressure of recent losses, and under whose flags. Zero for a role the system has
    // nowhere to put. A hot system is topped up towards it a couple of ships a pass; a cold
    // one is simply that.
    struct Population
    {
        int traders = 0, miners = 0, police = 0, pirates = 0;
        // ...and before the pressure, by role (NpcRole's first four): what it keeps once the
        // losses are forgotten. What it holds above this leaves (#295).
        std::array<int, 4> keep{};
        FactionId          policeFaction = Factions::DefaultPolice();
        FactionId          tradeFaction = Factions::Unaffiliated();
    };
    // Where a system has room for each role: lane ends (stations and gates), belts, and
    // whether anywhere is dark enough for pirates (a belt, or a gate into danger). A hot
    // system reads it off the nodes it gathers; a cold one off its profile and its
    // neighbours, without walking its objects.
    struct Room
    {
        size_t lanes = 0, fields = 0;
        bool   dark = false;
    };
    static Room RoomOf(const SpawnNodes& nd);
    Room        RoomOf(const SystemState& st) const;
    Population  PopulationOf(const SystemState& st, const Room& room) const;

    // Ships leaving (#295): those on their way, by id and by when they arrive. Saved.
    std::map<long long, Migrant>           migrants_;
    std::set<std::pair<double, long long>> arrivals_;
    long long                              nextMigrant_ = 1;
    // What the director keeps in each system this pass, in the order of systems_: worked out
    // once by StepMigration and read by the cold director after it. Transient.
    std::vector<Population> passTargets_;
    // Once a pass, before the director: who leaves, who has gone, who has arrived.
    void StepMigration();
    // A migrant has come: a number in a cold system, a ship at the gate in a hot one.
    void ArriveMigrant(const Migrant& m);
    // --- Haulers (#343; Simulation_Haulers.cpp) ---
    // What a trader setting out from `from` to `to` should carry: the good whose bid there
    // beats the ask here by the most, as much of it as still pays. Nothing, where nothing does
    // or either end has no market.
    struct Haul
    {
        ResourceType good;
        int          amount = 0;
        double       gain = 0.0;  // what the trip makes, fees included
    };
    Haul BestHaul(const SystemState& from, const std::string& to) const;
    // A ship of `role` sets out from `st` for its neighbour `to`, carrying `haul`: off the
    // count here, on its way for `travel` seconds, then counted there. In a hot system a real
    // ship turns for the gate; false, and nothing changed, when there is none to send.
    bool SetOut(SystemState& st, const std::string& to, NpcRole role, FactionId faction,
                const Haul& haul);
    // Once a pass, after migration: in each system with a market and a trader, one moment in
    // every `haulEvery` seconds, a trader sets out with the haul that pays best.
    void StepHauls();
    // Goods a hauler has brought, into the system's market.
    void Unload(SystemState& st, ResourceType good, int amount);
    // A hauler that came into a hot system: it carries its cargo to a market station.
    void LoadArrival(SystemState& st, NpcShip& ship, ResourceType good, int amount);
    // Haulers in a hot system that have reached a market, or whose time is up, unload; with
    // `all`, every one (the system is cooling, and its ships are counted out).
    void DeliverCargo(SystemState& st, bool all);
    // A hauler destroyed with goods aboard leaves them in a wreck where it was.
    void LeaveCargoWreck(SystemState& st, const NpcShip& ship);
    // A ship of `role` made in a hot system at `at`, given the work its role does there.
    void SpawnRoleAt(SystemState& st, NpcRole role, FactionId faction, Vector2 at);
    // A cold system's coarse pass (#295): what its ships would have done to each other in
    // the time, as expected losses -- counted as losses are in a hot one -- and then the
    // spawn director's arithmetic in place of its ships.
    void ColdLosses(SystemState& st);
    // `target` is what PopulationOf says for it this pass (StepMigration worked it out).
    void ColdTopUp(SystemState& st, const Population& target);
    // `seconds` of what a system's belts, planets and stations do to its market, and of the
    // trade nobody flies (#343): the same arithmetic hot or cold, in the coarse pass.
    void StepMarket(SystemState& st, float seconds);
    // A hot system nobody has been in for COOL_AFTER: its ships are counted into the
    // aggregate and let go.
    void Cool(SystemState& st);
    bool allHot_ = false;
    bool devCommands_ = false;  // --dev (#341)

    // A system's static layer was just built from its document: give the objects that may
    // change their keys, then replay what the save said had changed (#38).
    void KeyWorldObjects(SystemState& st);
    void ReplayChanges(SystemState& st);
    // Rebuilds neighbors_ from universe_.links; called wherever the links change.
    void IndexLinks();
    // Every market's base prices from its system's remoteness (#342 S5), counted afresh along
    // the links as they are now. After the links change, before the next macro pass reads it.
    void                       PriceMarkets();
    std::map<std::string, int> remoteness_;           // by system id; absent: none reaches it
    bool                       marketsStale_ = true;  // the links changed since PriceMarkets
    std::map<int, std::string> natives_;              // an open slot's natives, as JSON

    WorldLoader::Universe              universe_;
    bool                               hasRegion_ = false;
    uint64_t                           regionSeed_ = 0;
    std::string                        worldId_;     // (#392) empty: not a saved world
    std::map<std::string, std::string> regionDocs_;  // generated system documents, as JSON (#140)
    std::set<int>                      openSlots_;   // skeleton slots attached (#342)
    Gen::GalaxySkeleton                skeleton_;    // laid out by OpenGalaxy (#342 S2)
    std::vector<StartPoint>            starts_;
    // Admin packs (S7; Simulation_Packs.cpp).
    std::vector<std::shared_ptr<const Gen::Pack>> packs_;
    std::vector<Gen::Woven>                       woven_;
    std::vector<std::string>                      packProblems_;
    std::set<int>                                 packSlots_;
    const Gen::Pack*                              PackById(const std::string& id) const;
    // A new world's region packs, each into a free slot beside what is open, logged.
    void WeaveRegionPacks();
    // A region pack's region from a saved world's log: sealed, or woven again.
    bool OpenPackRegion(int slot, const std::string& id);
    // The system packs that go into `region`, just generated.
    void ApplySystemPacks(Gen::Region& region);
    // The admin's starts in a region just hung in the galaxy.
    void        AddStarts(const Gen::Region& region);
    void        SavePacks(nlohmann::json& j) const;
    static void ReadPacks(const nlohmann::json& j, std::vector<Gen::Woven>& out);
    // Hangs a generated region in the galaxy: its index entries, links and documents, and
    // the mouth of its wormhole beside home.
    void AddRegion(const Gen::Region& region);

    // Growth (#342 S3; Simulation_Tide.cpp).
    std::string                  dataDir_;  // OpenGalaxy's: pins and systems for a region opening
    std::string                  sealDir_;
    std::vector<RegionOpening>   openings_;
    std::map<int, std::string>   sealedIn_;     // read from files for a log replay, not yet used
    std::map<int, RegionTide>    tides_;        // saved
    std::set<std::string>        explored_;     // systems a player has been in (saved)
    std::set<std::string>        surveyed_;     // ...and those a faction surveyed (saved)
    std::map<int, std::set<int>> playersSeen_;  // sessions per region this period
    std::vector<int>             openedNow_;
    int                          tidePasses_ = 0;
    int                          maxRegions_ = 0, maxSystems_ = 2000;
    double                       surplusTide_ = 0.0;
    bool                         growthPaused_ = false;
    // The region of `slot`, made: read from its sealed file when the log replay has one,
    // else generated by `params` and sealed (kept, and written under SealDir).
    Gen::Region MakeSealed(int slot, const Gen::RegionParams& params);
    void        Seal(const Gen::Region& region);
    // An open region's systems and where they lie, as far as Gen::MouthSystem asks.
    Gen::Region RegionView(int slot) const;
    // A centre, made and hung in the galaxy with its start.
    void OpenCentre(int slot);
    // The opening appended to the log, by whom and why.
    void LogOpening(int slot, const std::string& mouth, const std::string& by,
                    const std::string& why);
    // Systems a region opened while the world runs: their aggregates, their static layers,
    // and the gate in the parent's mouth system, told to whoever stands there.
    void MaterializeOpened(int slot);
    // What the chronicle says of who lives beyond a mouth just opened (#342 S5): the region's
    // natives and where they trade. Empty for a region without any.
    std::string NativesLine(int slot) const;
    void MaterializeSystem(const WorldLoader::SystemInfo& info, const std::string& systemsDir);
    // The free slot a mouth from `fromSlot` opens into: its lowest-numbered free neighbour
    // that is not a centre; -1 for none.
    int NextSlotFrom(int fromSlot) const;
    // Opens the next slot from `fromSlot` now, and says so in the chronicle; -1 when it
    // could not.
    int OpenNext(int fromSlot, const std::string& by, const std::string& why);
    // Once a coarse pass: who is where; once a period, the tide and what it opens.
    void StepTide(double passSeconds);
    // A region's tide, begun where the seed says for its slot if it has none yet.
    RegionTide& TideOf(int slot);
    // A faction surveyed `systemId`: the tide of its region feels it.
    void NoteSurvey(const std::string& systemId);
    // How far an edge region's tide is staggered from the rest: the seed's, per slot.
    double StrainOf(int slot) const;
    // The share of a region's systems explored, faction surveys at their weight.
    double ExploredShare(int slot) const;
    // The params region `slot` is generated with, opened from `homeId` (empty for a centre):
    // its kind and law from the skeleton, its place and its wormhole's from home. False when
    // home is not a system of this galaxy. `homeDoc` must outlive the generation.
    bool ParamsFor(int slot, const std::string& homeId, const std::string& systemsDir,
                   Gen::RegionParams& params, nlohmann::json& homeDoc) const;
    // Mouths and Anchors (#342 S4).
    bool topologyChanged_ = false;
    // The routes changed: the host is told, and every pilot's map is built again (#431).
    void TopologyChanged()
    {
        topologyChanged_ = true;
        PublicChartsChanged();
    }
    RegionOpening* LiveOpening(int slot);
    // Where a mouth from `fromSlot` leads: the region that drifted from it, if one did --
    // the next mouth from a parent finds it again -- else its next free slot; -1 for none.
    int MouthTargetFrom(int fromSlot) const;
    // The opening whose wormhole joins systems `a` and `b`, either way round; null for none.
    RegionOpening* CrossingOf(const std::string& a, const std::string& b) const;
    // A mouth's two gates as its opening says they are: unstable, anchored and named, or the
    // gates it was made with. In a system being built (no deltas), or live (deltas).
    void DressCrossing(JumpGate& g, const RegionOpening& o) const;
    void DressCrossings(SystemState& st);
    void RedressMouth(const RegionOpening& o);
    // The route between a region and its parent, cut and made again: the link in the index
    // and the two gates, told to whoever stands there.
    void CutMouth(const RegionOpening& o);
    void RestoreMouth(const RegionOpening& o);
    // Once a pass: mouths failing, collapsing; anchors finished at them.
    void StepMouths();
    // A drifted region found again by the next mouth from its parent.
    int  ReopenRegion(int slot, const std::string& by, const std::string& why);
    void CollapseMouth(RegionOpening& o);
    // An Anchor of `owner` finished at the mouth of `o` (S4), or lost from it.
    void AnchorMouth(RegionOpening& o, const std::string& owner);
    void AnchorLost(SystemState& st, const std::string& owner, const std::string& name,
                    const std::string& key, Vector2 at, bool wasSite, Loss cause,
                    FactionId attacker);

    // An Anchor standing at either end of `o`'s mouth -- finished, or (`built` false) going up
    // as well -- whose owner; empty for none.
    std::string AnchorAt(const RegionOpening& o, bool built = true) const;
    // Where the gate in `systemId` to `dest` is, built or not -- a collapsed mouth's gates are
    // where they will be when it reopens. False when the system has no such gate.
    bool CrossingGateAt(const std::string& systemId, const std::string& dest, Vector2& at,
                        float& size) const;
    // A faction's Anchor convoy has come to the mouth: the site goes down beside it. False,
    // and the stock goes back, when the mouth is not one to anchor any more.
    bool LayAnchorSite(Plan& p);
    void ResolveAnchor(const Plan& p);
    // Who, by an owner string: a faction's name, or the pilot's.
    std::string OwnerName(const std::string& owner) const;

    // The archive (#342 S4; Simulation_Drift.cpp).
    std::map<int, ArchivedRegion> archives_;
    int                           nextArchive_ = 1;
    // A region coming back: what AttachRegion makes in place of the slot's own (null: its own),
    // and what is restored into its systems when they are built, by system id.
    const Gen::Region*                 returning_ = nullptr;
    std::map<std::string, std::string> returnState_;  // JSON, as text
    // Once a period, at the cap: regions nobody comes to are warned, then drift away.
    void StepDrift();
    bool ArchiveRegion(int slot, std::string& why);
    // Archive `id` brought back into `slot`, its wormhole from region `fromSlot`: decayed,
    // renamed for its new place. The slot, or -1.
    int BringBack(int id, int slot, int fromSlot, const std::string& by, const std::string& why);
    // Whether a player's structure stands anywhere in a region.
    int PlayerStructuresIn(int slot) const;
    // What a save keeps of a system's static layer (#38): what was taken away, changed and
    // added. Null when nothing was.
    nlohmann::json ChangesOf(const SystemState& st) const;
    void           SaveArchive(nlohmann::json& j) const;
    void           LoadArchive(const nlohmann::json& j);

    bool materialized_ = false;  // MaterializeAllSystems has run

    void                               SaveGrowth(nlohmann::json& j) const;
    void                               LoadGrowth(const nlohmann::json& j);
    std::map<std::string, SystemState> systems_;  // state by system id
    int                                agentIdCounter_ = 0;
    // The links, by system (#295): what Neighbors reads.
    std::map<std::string, std::vector<std::string>> neighbors_;
    // Wormhole mouths, as JSON, by the system they were opened from -- hand-written or
    // generated: a region's own documents are never changed by another region opening.
    std::map<std::string, std::vector<std::string>> wormholes_;

    // One per connected player (#3). A std::map because the verbs take a ClientSession&,
    // and a session must not move under one while the world is being stepped.
    std::map<int, ClientSession> sessions_;
    int                          sessionIdCounter_ = 0;

    // What the world remembers about a structure's owner while they are away (#340): their
    // standings as of when they were last here -- an outpost firing on its owner's enemies
    // needs to know who those are with nobody to ask -- and the crimes their structures
    // committed since, charged when they are back. Saved with the world, by account name.
    struct OwnerBook
    {
        bool               seen = false;  // standings noted at least once
        PerFaction<float>  standing;
        PerFaction<float>  owedStanding;
        PerFaction<double> owedBounty;
        // Fees their markets took while they were away (#438), paid at login -- less any
        // licence's rent taken meanwhile, which may leave it below zero (#431 slice 6).
        double owedMoney = 0.0;
        // Their money as of when they were last on, so rent can be taken while they are away
        // within what they had (#431 slice 6), and their licences, by system.
        double                                money = 0.0;
        std::map<std::string, Corps::Licence> licences;
    };
    std::map<std::string, OwnerBook> owners_;
    // Attacks under way on players' structures (#431 C2), saved with the world so one that
    // a restart interrupts is still told; and what nobody was on to read, for the host.
    std::map<std::string, AttackEpisode>           attacks_;
    std::vector<std::pair<std::string, Ev::Event>> absent_;
    void NoteAttack(const std::string& owner, const std::string& key, const std::string& name,
                    const std::string& system, const std::string& by, float hull, bool destroyed);
    void EndAttack(const AttackEpisode& a, bool destroyed);
    // An entry for `owner` and their corporation: told to whoever is on, kept for the rest.
    // `onlyOn`: only to whoever is on, nothing kept (an attack starting).
    void TellOwner(const std::string& owner, const std::string& yours, const std::string& theirs,
                   bool onlyOn, Ev::Kind kind = Ev::Kind::StructureAttacked);
    // A party's licence set or dropped (#431 slice 6), wherever the party keeps it; and a word
    // to whoever pays it: the pilot, or the corporation's journal.
    void SetLicence(const std::string& party, const std::string& system, const Corps::Licence& l);
    void DropLicence(const std::string& party, const std::string& system);
    void TellParty(const std::string& party, const std::string& text);
    // A period's rent taken from the party and paid to the holder; false, nothing taken, when
    // it cannot pay -- `why` says so.
    bool PayRent(const std::string& party, const std::string& holder, double rent,
                 std::string& why);
    // An entry in `account`'s own journal: now, if they are on, else waiting in the pending
    // part of it (absent_) until their next login.
    void TellAccount(const std::string& account, Ev::Kind kind, const std::string& text);
    void KeepForAbsent(const std::string& account, Ev::Kind kind, const std::string& text);
    // Gives `s` what their corporation's journal holds that they have not read.
    void           ReadCorpJournal(ClientSession& s);
    nlohmann::json OwnersJson() const;
    void           OwnersFrom(const nlohmann::json& j);

    // Roaming holes (S9): what the epochs decided, the last epoch decided, and which
    // signatures somebody has already been credited with finding (#346).
    std::vector<RoamingHole> holes_;
    int                      holeEpoch_ = -1;
    std::set<std::string>    foundSignatures_;
    void                     SaveHoles(nlohmann::json& j) const;
    void                     LoadHoles(const nlohmann::json& j);
    RoamingHole*             MutableHole(const std::string& id);
    // What the ship this pilot flies weighs, by its design; 0 when it cannot be derived.
    double ShipMass(const ClientSession& s) const;
    void   OpenHole(RoamingHole& h);
    void   CloseHole(RoamingHole& h, const std::string& why);
    // Builds a hole's gates where a system has its static layer and they are not standing.
    void BuildHoleGates(RoamingHole& h);
    // The name a hole's gates wear: its class and how it is.
    std::string HoleName(const RoamingHole& h) const;
    // Someone resolved a signature; the first to do so is news.
    void CreditFind(ClientSession& s, const std::string& key, const std::string& what,
                    const std::string& systemId);
    // A signature is this pilot's from now on: in their account, in a delta of their own, and
    // news if nobody had it. Returns the journal's line for it: "KQX-417: a hidden base -- Den".
    std::string Resolve(ClientSession& s, const SystemState& st, const Entity& e);

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
    // Knowledge (#431). `publicRev_` counts changes to what everyone's map is built from --
    // the public charts, names, the galaxy's shape; `knowledgeSerial_` hands out the
    // revisions of what pilots have found (ClientSession::knownRev).
    long long publicRev_ = 1;
    long long knowledgeSerial_ = 0;
    // A pre-#431 world's finds (account -> systems), given to their discoverers when they
    // next log in; saved in world.json until then.
    std::map<std::string, std::set<std::string>> legacyKnown_;
    void                                         PublicChartsChanged() { publicRev_++; }
    // Whether `faction` may chart `systemId` (ChartBy): not public by place, not charted
    // yet, and not its own natives' home.
    bool CanChart(const std::string& systemId, FactionId faction) const;
    // The charts' fields alone, as a save or the archive restores them: no first contact.
    void MarkCharted(SystemAggregate& a, FactionId faction);
    // A system's name, as its people or its first charter give it: kept like a player's
    // (`givenName`), on every map that has it, and on its planets.
    void CallSystem(SystemState& st, const std::string& name);
    // Folded names already taken in the world, for a new one to differ from (N3): every
    // system's designation, id and given name, and every name an open region's natives have
    // for their systems, revealed or not.
    std::set<std::string>    TakenNames() const;
    std::vector<std::string> blockedNames_;  // SetBlockedNames: not world state, never saved
    // The system `which` names: an id, or a designation or name in any case. Empty if none.
    std::string SystemNamed(const std::string& which) const;
    void        SaveLegacyKnown(nlohmann::json& j) const;
    void        LoadLegacyKnown(const nlohmann::json& j);
    // After an account is read: any pre-#431 finds of theirs, the system they are in, and
    // their charts onto their corporation's.
    void LoadKnown(ClientSession& s);
    // An index of the public charts, and of `s`'s when given, with the systems a gate from
    // any of those leads to as uncharted stubs.
    WorldLoader::Universe BuildIndex(const ClientSession* s) const;
    // What `s` has found changed: systems or holes.
    void TouchKnown(ClientSession& s) { s.knownRev = ++knowledgeSerial_; }
    void TouchHoles(ClientSession& s)
    {
        s.routesChanged = true;
        TouchKnown(s);
    }
    // Whether a system is public by where it is: hand-written space and the centres.
    bool                 PublicByPlace(const std::string& systemId) const;
    static constexpr int SETTLE_STEPS = 3;
    unsigned int         rng_ = 0x1234567u;  // RNG state

    std::vector<std::string> events_;  // recent galaxy events (capped)

    // What each faction knows, and what it has set in motion (#295). Saved. Seeded with
    // its holdings and their neighbours the first time the faction step runs, when the
    // static layers they are read from exist -- or, for an older save, the first time
    // after it is loaded.
    PerFaction<FactionMind> minds_;
    bool                    mindsSeeded_ = false;
    std::map<int, Plan>     plans_;
    // Plans by when they are due, then by id: a pass pops what is due and looks at
    // nothing else. Not saved -- remade from plans_.
    std::set<std::pair<double, int>> due_;
    int                              nextPlanId_ = 1;
    // Delivery contracts (#343, #407), by id. Saved. When markets last looked at what they are
    // short of, as a stretch of the contracts rules' `every`: transient, so a restart looks.
    std::map<int, Contract>     contracts_;
    int                         nextContractId_ = 1;
    double                      contractSlot_ = -1.0;
    std::vector<ChronicleEntry> chronicle_;
    long long                   chronicleSeq_ = 0;

    // When each structure next has something happen to it -- it is finished, or its time is
    // up -- as (instant, entity id, system). StepStructures pops what is due and touches
    // nothing else (#295). Not saved: remade when the static layers are built, from the time
    // lines the structures carry. An entry whose structure has gone is skipped.
    std::set<std::tuple<double, int, std::string>> structureDue_;
    // Puts a structure's coming instants on the queue, and a wreck's end (#41); anything else
    // is ignored. Whatever changes a time line after it is added schedules it again.
    void ScheduleStructure(const std::string& systemId, const Entity& e);

    void SeedMinds();
    void Think(FactionId f);
    void ResolveDuePlans();
    void ResolveSurvey(const Plan& p);
    // A faction lays an outpost site in `to`, coming from `from`, and pays for it (#295).
    // False when the system turns out not to be one it may settle, or there is no room.
    bool Settle(FactionId f, const std::string& from, const std::string& to);
    void ResolveSettle(const Plan& p);

    // --- Keeping outposts, and sieges (#295, slice 5; Simulation_Sieges.cpp) ---
    // Once a period: every finished faction outpost is paid for from its faction's stock or
    // is not, and mends or decays accordingly.
    void StepUpkeep();
    // Once a pass: each siege wears its outpost down by the arithmetic both a cold and a hot
    // system read, counts the ships a player has shot down, and ends when it has won or lost.
    void StepSieges();
    // `f` sends what `lent` lends against `against`'s outpost `site` in `to`, coming in from
    // its neighbour `via`, and pays. The force sets out now and arrives when the farthest has.
    void BeginSiege(FactionId f, const std::vector<Plan::Lent>& lent, const std::string& via,
                    const std::string& to, FactionId against, const std::string& site,
                    const std::string& evicting = std::string());
    // The pooled force reaches the outpost: it is strength there from now on, and ships where
    // somebody is to see them. False when the outpost has gone since: it turns back.
    bool ArriveSiege(Plan& p);
    // A siege's time is up with the outpost still standing.
    void ResolveSiege(const Plan& p);
    // What is left of a siege goes home: its ships, where they are real, leave the system, and
    // its strength sets out for the holdings that lent it, each its share of what survived.
    void RecallSiege(const Plan& p);
    // A contingent is home: what it brought back is that holding's strength again.
    void ResolveReturn(const Plan& p);
    // A hot system's siege made real: warships of the besieger at the outpost -- or, for one
    // that has just arrived, at the gate it came through.
    void FieldSiege(SystemState& st, Plan& p, bool arriving);
    void FieldSieges(SystemState& st);
    // The outpost a save knows by `key` in a system, if it still stands.
    Structure* StructureByKey(SystemState& st, const std::string& key) const;
    // A faction's finished outpost is gone: a claim on the system that rested on it goes, and
    // one that rests on something else as well is weaker.
    void ShakeClaim(SystemState& st, FactionId f);
    bool HitStructure(SystemState& st, int id, float damage, ClientSession* by, Loss cause,
                      FactionId attacker);
    // What is left of a lent force goes home (#295): each holding that lent it its share of
    // `p.force`, as many jumps away as it came, `hop` seconds a jump -- a Return each.
    void SendHome(const Plan& p, double hop);
    // The police answer from here, now (#431 slice 9): the system's own free police of the
    // law warp beside `by`, and its stations launch what the faction can spare. How many of
    // each is the band's. Returns how many came.
    int AnswerHere(SystemState& st, ClientSession& by, FactionId law,
                   const WorldRules::Law::Band& band);
    // Police of `law` from systems within the band's reach, lent: taken off each home's ships
    // and presence now, and a Response that brings them to `st`. How many set out.
    int CallNextDoor(SystemState& st, const ClientSession& by, FactionId law,
                     const WorldRules::Law::Band& band);
    // Up to `want` free police of `law` taken out of `home`, by its ships where it is hot and
    // its numbers where it is cold -- the same count either way -- with the presence behind
    // them. How many were taken.
    int TakePolice(SystemState& home, FactionId law, int want);
    // A Response's police arrive and, where somebody is to see them, are ships beside whoever
    // they answer.
    void FieldResponse(SystemState& st, Plan& p);
    // Once a pass: Responses arrive, and the ships a player has shot down are lost to them.
    void StepResponses();
    // A Response's time is up: its ships leave and what is left of it goes home.
    void RecallResponse(const Plan& p);
    // --- Jobs (#345; Simulation_Jobs.cpp) ---
    // A plan's job handed in: paid out of the poster's stock, and a delivery fed to its site.
    bool CompleteJob(ClientSession& s, int index);
    // What a job paid comes off its poster's stock (#345).
    void PayJob(const Mission& m);
    // A job ended undone, with why; `penalty` when it was the pilot's to do.
    void FailJob(ClientSession& s, int index, const std::string& why, bool penalty);
    // A delivery takes the jobs rules' share off the time the plan's site has left.
    void SpeedSite(Plan& p);
    // The escort strength pilots with a convoy's escort jobs lend it in `systemId`.
    float EscortsWith(const Plan& p, const std::string& systemId) const;
    // A siege is over: break-the-siege jobs that helped lift it are done, the rest are not.
    void SiegeOver(const Plan& p);
    // --- Contracts (#343, #407; Simulation_Contracts.cpp) ---
    // The contracts on the board of `giver`, a station within `near` (systems by jumps).
    void ContractsPostedAt(const ClientSession& s, const Station& giver,
                           const std::map<std::string, int>& near, std::vector<Mission>& out) const;
    // A pilot takes a contract off the boards: false, said why, when it is gone or taken.
    bool TakeContract(ClientSession& s, const Mission& m);
    // A contract handed in: the goods into the market, the price to the pilot.
    bool CompleteContract(ClientSession& s, int index);
    // A contract ends undone: what was set aside for it is its owner's again.
    void EndContract(int id);
    // A trade's fee, to the owner of `st`'s market (#343, #348): a hauler's, as a pilot's.
    void PayFee(const SystemState& st, double fee);
    void SaveContracts(nlohmann::json& j) const;
    void ReadContracts(const nlohmann::json& j);
    // --- Expeditions, growing stations and the builder convoy (#342 S6; Simulation_Growth.cpp) ---
    // A mouth into `slot` opened: the factions within the expedition rules' reach of it hear
    // what is beyond -- intel on the entry system -- and are drawn that way for a while.
    void SendWordOfMouth(int slot);
    // What going from holding `home` to `to` is worth to `f` on top, while word of a mouth
    // draws it: `toward` is, per system, how many jumps from a mouth it has heard of, and
    // `drawn` the regions beyond them.
    struct Drawn
    {
        std::map<std::string, int> toward;
        std::set<int>              regions;
    };
    Drawn        DrawnBy(FactionId f);
    static float DrawOf(const Drawn& d, const std::string& home, const std::string& to);
    // What `f` would grow its outpost in `systemId` into, and what that is worth to it: empty
    // when nothing is needed there.
    std::string GrowthFor(FactionId f, const SystemState& st, const Structure& outpost,
                          float& worth);
    std::map<std::string, int> marketJumps_;  // MarketJumps, as of `marketPeriod_`
    int                        marketPeriod_ = -1;
    // A plan's convoy sets out from where the faction makes things (ChartConvoy): its way
    // there and its escort -- or it is held back, for the stock to pay for what an earlier
    // one lost, or for better odds. False when it has no way to get there.
    bool DispatchConvoy(Plan& p);
    // What `f` believes is waiting for its convoy in `systemId`, in ships: everyone hostile it
    // saw there, a ship more where it has never looked, and the memory of a loss there. With
    // `truth`, what is there now -- what a convoy crossing it meets. Its security, as seen, in
    // `security` when asked.
    float ConvoyDanger(FactionId f, const std::string& systemId, bool truth,
                       float* security = nullptr) const;
    // A convoy of strength `strength` crossing a system of `danger` ships at `security`: the
    // chance it is attacked, and the share of its freight an attack takes on the average.
    static float AmbushChance(float danger, float security);
    static float AmbushHarm(float strength, float danger);
    // Once a pass: every convoy moves on, fights what it meets, arrives, turns back or is lost.
    void StepConvoys();
    // The convoy has crossed the system it is in: whatever is hostile there falls on it, or not.
    // False when that was the end of it.
    bool CrossSystem(Plan& p, SystemState& st);
    // What the convoy carries is worth this much of the faction's stock: the plan's price.
    static float FreightPrice(const Plan& p);
    // Turned back with what is left: the escort and the freight go home, another sets out later.
    void TurnBackConvoy(Plan& p, const std::string& where, const std::string& why);
    // A convoy's end, counted for the report.
    void                       TallyConvoy(const Plan& p, long long ConvoyTally::* what);
    ConvoyTally                convoyTally_;
    std::map<int, ConvoyTally> convoyRegions_;
    // Its ships, where the system it is in is hot: the hauler and the escort, on their way
    // between where they came in and where they go out -- or, just come in, at the gate.
    void FieldConvoy(SystemState& st, Plan& p, bool arriving);
    void FieldConvoys(SystemState& st);
    void UnfieldConvoy(Plan& p);
    // Lost on the way: the escort that is left goes home, and another sets out later.
    void LoseConvoy(Plan& p, const std::string& how);
    // Come to its site: the escort goes home, and the Anchor or the upgrade begins. False when
    // there is nothing to build any more, and the plan is over.
    bool ArriveConvoy(Plan& p);
    // What a convoy's escort lends, and the escort that goes home after it.
    void SendHome(const Plan& p, int hops);
    // The Upgrade's outpost becomes its site.
    bool StartUpgrade(Plan& p);
    void ResolveUpgrade(const Plan& p);
    // A finished site of a station's blueprint becomes a station of its builder's faction.
    void FinishStation(SystemState& st, int id);

    // `f` takes a system: its controller, and the security a change of hands brings. Taking
    // it is reaching it: `f` charts it (ChartBy).
    void TakeControl(const std::string& systemId, FactionId f);
    void Record(const std::string& kind, std::optional<FactionId> faction,
                const std::string& system, const std::string& text);
    // News: the feed every client shows, and the history, which keeps who and where.
    void        Announce(const std::string& kind, std::optional<FactionId> faction,
                         const std::string& system, const std::string& text);
    std::string DescribeSurvey(const Intel& seen, FactionId by) const;

    // What attacking something of a lawful faction's costs the attacker with that faction:
    // per hit, and more for the kill. One table for ships and structures alike (#41).
    static void ChargeAttack(ClientSession& s, FactionId victim, bool killed);

    void        SeedAggregate(SystemState& st, const WorldLoader::SystemInfo& info);
    std::string SystemName(const std::string& id) const;
    void        PushEvent(const std::string& msg);
};
