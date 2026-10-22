#pragma once

#include "economy/Resource.h"
#include <string>
#include <utility>
#include <vector>

// How a player makes something (#39): which archetype it builds, what it costs, how long it
// takes and where it may stand. Read from data/blueprints.json.
//
// A blueprint is apart from the archetype on purpose. The archetype says what a thing IS
// and is the same however it came to exist; the blueprint says how one is MADE, and one
// kind of object may be made more than one way -- a cheap kit and a slow one, or later a
// design a player drew (M8). An archetype a blueprint names must declare `buildable`.
//
// Shared by the server, which decides, and by both clients, which show what can be built
// and refuse the obvious before asking -- the server checks everything again.
struct Blueprint
{
    std::string id;         // "beacon" -- what a command names
    std::string name;       // "Beacon" -- what a player is shown, and a new one is called
    std::string archetype;  // what it becomes once built: a Structure archetype -- or, for an
                            // upgrade, a Station's

    // Taken from the hold when the site is laid down, all of it at once: what is committed
    // is committed, which is what makes a site worth defending.
    std::vector<std::pair<ResourceType, int>> cost;

    float buildSeconds = 0.0f;  // world seconds from site to finished object
    float lifetime = 0.0f;      // world seconds it stands once finished; 0 -- until removed

    // What it takes to shoot the finished thing down (#39). Not optional: anything buildable
    // is killable, or the map fills up and never changes again. A site has a share of it
    // that grows as it goes up (Blueprints::Destruction).
    float hull = 0.0f;
    // Its tier (#431 D1), one of the file's `tiers`, and what that tier mends: a share of `hull`
    // per world second, for as long as it stands finished. Fire below that rate is undone as
    // fast as it lands, so the tier is what decides how many guns it takes -- a beacon falls to
    // one pilot, an outpost to a few, a station's frame only to a fleet.
    std::string tier;
    float       mend = 0.0f;
    // The wreck it leaves when destroyed: a Derelict archetype, or empty for something too
    // small to leave anything worth searching -- a buoy goes up in a flash.
    std::string wreck;

    // Where it may go. Each is a rule a player can be told in a sentence when it refuses.
    float reach = 0.0f;          // at most this far from the ship laying it down
    float clearance = 0.0f;      // gap to the edge of any station, gate, wreck or structure
    float bodyClearance = 0.0f;  // gap to a star's or planet's surface, and to the path one
                                 // that moves sweeps -- it would come round and through it
    int perSystem = 0;           // of this blueprint in one system, everyone's; 0 -- no cap
    // Only within this of an unstable wormhole mouth's edge (#342 S4): what an Anchor is
    // built at. 0 -- anywhere.
    float mouth = 0.0f;

    // Who may lay one down (#295): "builders" in the file, ["player"] when it says nothing.
    // An outpost is a faction's claim on a system and is not offered to a player yet.
    bool byPlayers = true;
    bool byFactions = false;

    // An upgrade (#342 S6): built onto a finished structure of one of these blueprints, which
    // becomes its site rather than a new one standing beside it -- an outpost growing into a
    // market, a market into a station. Empty for anything laid down on its own. Only an upgrade
    // may build a Station archetype: its site is a structure, and once finished it is a station
    // of its builder's faction.
    std::vector<std::string> onto;
};

namespace Blueprints
{
// Reads data/blueprints.json. Load Archetypes first: every blueprint is checked against the
// archetype it names. False with Error() set on anything wrong, and the previous contents
// are left in place -- a blueprint that cannot be read is content the author can fix, and
// one that silently costs nothing is not.
bool Load(const std::string& path);

const Blueprint* Find(const std::string& id);  // any blueprint; null for an unknown id
// The first blueprint that builds this archetype, for a structure that does not say which
// one made it (a save from before it did); null when none does.
const Blueprint* Building(const std::string& archetype);
// What a player may build, in the file's order: every menu and tool lists this. A blueprint
// only factions build is found by Find and listed nowhere.
const std::vector<Blueprint>& All();

// How many structures one account may have standing at once, everywhere (#41 will replace
// this with real limits; until then it is what stops one player filling a system).
int PerAccount();

// Taking something down (#39, slice 2): what comes back, and what is left. One set for every
// blueprint, in the file's `dismantle` and `destruction` sections, because they are rules of
// the world rather than properties of a beacon.
struct Teardown
{
    // The share of the cost an owner gets back for dismantling a finished structure. A site
    // returns more the less of it is assembled: all of it at the moment it goes down, falling
    // in a straight line to this at completion -- what is still crated comes back whole, what
    // is bolted together comes apart at a loss.
    float refund = 0.5f;
    // A site's hull as a share of the finished thing's at the moment it goes down, growing to
    // all of it at completion: a frame half built is not as strong as the station.
    float siteHull = 0.25f;
    // The share of the cost a destroyed structure's wreck holds, in credits at the base
    // price -- less than the refund, so shooting your own down never beats dismantling it.
    float wreckShare = 0.25f;
    // World seconds a structure's wreck lies where it fell before it is gone (#41): long
    // enough to be found and searched, short enough that the map does not fill with ruins.
    // 0 -- it stays until somebody removes it.
    float wreckSeconds = 21600.0f;
};
const Teardown& Rules();

// Keeping a faction's outpost standing (#295, slice 5): the file's `upkeep` section. One set
// for every outpost, like the teardown rules. Applied once a faction period -- a minute -- by
// the coarse pass, so an outpost in a system nobody is in is kept, or let go, all the same.
struct Upkeep
{
    // What one outpost costs its faction's stock each period, as a share of what it cost to
    // build. Paid only for an outpost in a system the faction holds: one standing in somebody
    // else's system, or nobody's, is cut off and paid for by nobody.
    float share = 0.006f;
    // The share of its hull an outpost nobody supplies loses each period. At zero it is a
    // ruin, like one shot down -- slowly enough that the faction may yet win it back.
    float decay = 0.04f;
    // A supplied one mends at its tier's rate like any finished structure (Blueprint::mend);
    // an unsupplied one does not mend at all.
};
const Upkeep& UpkeepRules();

// A faction attacking another's outpost (#295, slice 5): the file's `siege` section. A siege is
// strength a faction sends to stand at the outpost and wear it down; where nobody is it is
// arithmetic, where somebody is it is ships. Both read these numbers.
struct Siege
{
    float cost = 0.5f;       // stock it takes to mount one, as a share of what an outpost costs
    float seconds = 900.0f;  // how long the besiegers stay before they give up
    float jitter = 120.0f;   // ...plus up to this much more, drawn from what is decided
    // The most of its capacity a faction sends, out of what its holding can spare: a siege
    // leaves home as guarded as a move does.
    float commit = 0.5f;
    // What a besieging ship takes off a wall is not here: it is the faction's own warship
    // design's guns (Ships::WarshipDps, Simulation::WarshipDps).
    // How many besiegers each defending ship ties up: what is left over fires at the outpost.
    float defence = 0.15f;
    // The share of a siege's seconds a ship spends trading fire with one ship of the other
    // side. What that costs each side is the guns' arithmetic (#186): the other side's
    // design's dps over its own design's hull (Simulation::FighterOf), so a faction with the
    // better warship loses fewer. At today's designs, about 0.0002 ships a second each way.
    float attrition = 0.0005f;
    // What losing an outpost costs the faction that held the system, as a share of its
    // strength there: a claim that rested on it is gone, and one that did not is weaker.
    float shock = 0.5f;
    // Pooling (#295): the holdings that may lend a siege ships are those within `reach` jumps
    // of its target, and each jump takes `hop` seconds, there and back again. All of them
    // together send at most `pool` of the faction's capacity.
    float reach = 3.0f;
    float hop = 120.0f;
    float pool = 1.5f;
};
const Siege& SiegeRules();

// What an owner gets back for dismantling it, per resource, at `progress` 0..1 of the build
// and `hullFraction` of its hull still standing -- what was shot off is gone. Rounded down,
// so taking a thing apart never makes material. One function, for the server that pays it and
// for a client that shows it before asking.
//
// `earlier` are the tiers it was built up through before `bp` (#438), each finished, so each
// returns the finished share of its cost: an outpost grown into a market and a station gives
// back something of all three, not only the station's frame. Summed before rounding down.
std::vector<std::pair<ResourceType, int>> Refund(const Blueprint& bp, float progress,
                                                 float                                hullFraction,
                                                 const std::vector<const Blueprint*>& earlier = {});

const std::string& Error();
}  // namespace Blueprints
