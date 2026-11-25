#pragma once

#include "core/WorldLoader.h"
#include "sim/SaveSchema.h"

#include <map>
#include <set>
#include <string>
#include <vector>

// What the world keeps of its own growth (#342, #353): the log of every region that opened,
// each region's tide, what has been explored and surveyed, the cap, the archive of regions
// that drifted away and the renamings of charts that followed. `Simulation` decides when a
// region opens, drifts or returns; this is the state it does that on.
namespace Growth
{
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

// --- The archive (#342 S4; Simulation_Drift.cpp) ---
// A region that drifted away at the cap: its documents as they were sealed, and what had
// become of it -- what stood and lay in each system, the names players gave, its history.
struct ArchivedRegion
{
    int         id = 0;
    int         slot = 0;  // where it was
    double      archivedAt = 0.0;
    std::string text;  // the archive document, as JSON
    // The world version of the build that sealed it, for what its changes mean when it
    // comes back (#498): a member's structure before 18 is the corporation's.
    int version = Save::WORLD_VERSION;
    // Its geography as charts keep it (#431 slice 15): each system located, named and
    // linked, under its archived id (ArchivedId) -- what a pilot who knew it still sees,
    // marked drifted. Made from `text`, never saved apart from it.
    std::vector<WorldLoader::SystemInfo> charts;
    std::vector<WorldLoader::SystemLink> links;
};

// Every renaming of charts so far (#431 slice 15): a region's ids into the archive, or out
// of it into the slot it came back to (`back`: what was seen there is outdated). Sessions
// and corporations are renamed at once; an account saved before is renamed when it is
// read, from the number of renamings its save says it had seen.
struct ChartRename
{
    std::map<std::string, std::string> ids;
    bool                               back = false;
};

// --- The tide (#342 S3; Simulation_Tide.cpp, rules in WorldRules.h) ---
// What one region's tide is, and what it was made of since its last mouth.
struct RegionTide
{
    double tide = 0.0;
    double fromTime = 0.0, fromPlayers = 0.0, fromExplore = 0.0, fromSurveys = 0.0;
    int    surveys = 0;            // faction surveys of the region this period
    double lastMouthAt = -1.0e18;  // when it last opened one
};

}  // namespace Growth

// Plain state, as `Plan` is: the invariants (who may open, when a region drifts) are the
// simulation's rules, not this record's.
struct GalaxyGrowth
{
    using RegionOpening = Growth::RegionOpening;
    using ArchivedRegion = Growth::ArchivedRegion;
    using ChartRename = Growth::ChartRename;
    using RegionTide = Growth::RegionTide;

    // The log, in the order regions opened. Not cleared by a reset: the galaxy's.
    std::vector<RegionOpening> openings;
    std::map<int, std::string> sealedIn;  // read from files for a log replay, not yet used

    // The tide (saved), and what a player or a faction has been to.
    std::map<int, RegionTide>    tides;
    std::set<std::string>        explored;     // systems a player has been in (saved)
    std::set<std::string>        surveyed;     // ...and those a faction surveyed (saved)
    std::map<int, std::set<int>> playersSeen;  // sessions per region this period
    std::vector<int>             openedNow;
    int                          tidePasses = 0;
    int                          maxRegions = 0, maxSystems = 2000;
    double                       surplusTide = 0.0;
    bool                         growthPaused = false;

    // The archive: regions that drifted away at the cap, and the renamings of charts.
    std::map<int, ArchivedRegion> archives;
    int                           nextArchive = 1;
    std::vector<ChartRename>      chartRenames;  // saved with the archive
    // What is restored into a returning region's systems when they are built, by system id
    // (JSON, as text).
    std::map<std::string, std::string> returnState;

    // A new world's tide: nothing explored, nothing surveyed, no surplus. The log, the
    // archive and the cap are the galaxy's, set by OpenGalaxy.
    void ResetTide()
    {
        tides.clear();
        explored.clear();
        surveyed.clear();
        playersSeen.clear();
        surplusTide = 0.0;
        growthPaused = false;
        tidePasses = 0;
    }
};
