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
    std::string archetype;  // what it becomes once built: a Structure archetype

    // Taken from the hold when the site is laid down, all of it at once: what is committed
    // is committed, which is what makes a site worth defending.
    std::vector<std::pair<ResourceType, int>> cost;

    float buildSeconds = 0.0f;  // world seconds from site to finished object
    float lifetime = 0.0f;      // world seconds it stands once finished; 0 -- until removed

    // What it takes to shoot the finished thing down (#39). Not optional: anything buildable
    // is killable, or the map fills up and never changes again. A site has a share of it
    // that grows as it goes up (Blueprints::Destruction).
    float hull = 0.0f;
    // The wreck it leaves when destroyed: a Derelict archetype, or empty for something too
    // small to leave anything worth searching -- a buoy goes up in a flash.
    std::string wreck;

    // Where it may go. Each is a rule a player can be told in a sentence when it refuses.
    float reach = 0.0f;          // at most this far from the ship laying it down
    float clearance = 0.0f;      // gap to the edge of any station, gate, wreck or structure
    float bodyClearance = 0.0f;  // gap to a star's or planet's surface, and to the path one
                                 // that moves sweeps -- it would come round and through it
    int perSystem = 0;           // of this blueprint in one system, everyone's; 0 -- no cap

    // Who may lay one down (#295): "builders" in the file, ["player"] when it says nothing.
    // An outpost is a faction's claim on a system and is not offered to a player yet.
    bool byPlayers = true;
    bool byFactions = false;
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
};
const Teardown& Rules();

// What an owner gets back for dismantling it, per resource, at `progress` 0..1 of the build
// and `hullFraction` of its hull still standing -- what was shot off is gone. Rounded down,
// so taking a thing apart never makes material. One function, for the server that pays it and
// for a client that shows it before asking.
std::vector<std::pair<ResourceType, int>> Refund(const Blueprint& bp, float progress,
                                                 float hullFraction);

const std::string& Error();
}  // namespace Blueprints
