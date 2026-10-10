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

    // Where it may go. Each is a rule a player can be told in a sentence when it refuses.
    float reach = 0.0f;          // at most this far from the ship laying it down
    float clearance = 0.0f;      // gap to the edge of any station, gate, wreck or structure
    float bodyClearance = 0.0f;  // gap to a star's or planet's surface, and to the path one
                                 // that moves sweeps -- it would come round and through it
    int perSystem = 0;           // of this blueprint in one system, everyone's; 0 -- no cap
};

namespace Blueprints
{
// Reads data/blueprints.json. Load Archetypes first: every blueprint is checked against the
// archetype it names. False with Error() set on anything wrong, and the previous contents
// are left in place -- a blueprint that cannot be read is content the author can fix, and
// one that silently costs nothing is not.
bool Load(const std::string& path);

const Blueprint*              Find(const std::string& id);  // null for an unknown id
const std::vector<Blueprint>& All();

// How many structures one account may have standing at once, everywhere (#41 will replace
// this with real limits; until then it is what stops one player filling a system).
int PerAccount();

const std::string& Error();
}  // namespace Blueprints
