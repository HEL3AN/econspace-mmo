#pragma once

#include "entities/Entity.h"
#include "raylib.h"
#include <nlohmann/json_fwd.hpp>
#include <map>
#include <vector>
#include <memory>
#include <string>
#include <utility>

// Loading the game world from JSON files.
namespace WorldLoader
{
// One thing that stood in a system when it was last looked at (#431 slice 14): a station or a
// structure, as what it is, what it is called and whose -- a line of a summary, never the
// object, so what an account keeps of a system it saw is a few words and not a copy of it.
struct SeenObject
{
    std::string kind;  // "station", or a structure's archetype ("structure.outpost")
    std::string name;
    std::string owner;             // as a structure's: "faction:<id>", an account, or empty
    bool        market = false;    // a ship can trade there
    bool        building = false;  // a site still going up

    bool operator==(const SeenObject& o) const
    {
        return kind == o.kind && name == o.name && owner == o.owner && market == o.market &&
               building == o.building;
    }
};

// The observed layer of one system (#431 M1): what stood there the last time a pilot, their
// corporation or their structures were present, and when (world time). Geography is known for
// good and owner and security travel live with the galaxy statistics; this is the part that
// is only as true as the date beside it.
struct Sighting
{
    // A summary is bounded whatever a system holds: past this, only how many more.
    static constexpr size_t MAX_SEEN = 32;
    double                  at = -1.0;  // below zero: never observed
    std::vector<SeenObject> seen;
    int                     more = 0;  // ...stood there beyond MAX_SEEN
    // Seen before its region drifted away and came back renamed (#431 slice 15, M3): the
    // geography holds, what stood there must be looked at again. A fresh sighting clears it.
    bool outdated = false;

    bool Observed() const { return at >= 0.0; }
    // The same things stood there: the date alone is not news.
    bool SameAs(const Sighting& o) const { return seen == o.seen && more == o.more; }
};
// Compact JSON, for the account, corps.json and the wire alike.
nlohmann::json ToJson(const Sighting& s);
Sighting       SightingFrom(const nlohmann::json& j);
// A region's systems renamed (#431 slice 15): each sighting kept by a system of `ids` moves
// to its new id, and is marked outdated when `outdate`. Anything else stays as it was.
void RenameSightings(std::map<std::string, Sighting>&          sightings,
                     const std::map<std::string, std::string>& ids, bool outdate);

// A system's record in the galaxy index (data/universe.json).
struct SystemInfo
{
    std::string id;               // short identifier (= a gate's destination)
    std::string name;             // display name
    std::string file;             // file name under data/systems/
    Vector2     mapPos;           // position on the star map
    float       security = 0.5f;  // security level 0..1 (1 = safe)
    std::string owner;            // owning faction (id); empty — unclaimed system
    // Whether it is on the charts of whoever this index is for (#144, #431). An uncharted
    // system is known only to exist, because a gate from a charted one leads to it: where it
    // is on the map and what it is designated, never its security, owner or contents.
    bool charted = true;
    // A generated system's designation, when it has been given a name (#145): "W-3.2"
    // stays the way to tell two people mean the same place. Empty otherwise.
    std::string designation;
    std::string discoverer;  // who got there first, if anybody has
    // Whose charts it is on (#431): empty -- the public ones; "own" -- this
    // pilot's alone; "corp" -- a corporation member's. And the world faction that put it on
    // the public charts, by id, when one did.
    std::string knownBy;
    std::string chartedBy;
    // What this pilot last saw standing there, and when (#431 slice 14) -- theirs or their
    // corporation's, whichever is newer -- and whether it is live: they, a corporation member
    // or a structure of theirs is there now, so it is what stands there now. (Braced: an
    // aggregate initialised without them is not a missing initializer.)
    Sighting observed{};
    bool     observedLive{ false };
    // Its region drifted away (#431 slice 15, M3): cut off from the galaxy, or gone into the
    // archive. Kept on the charts of whoever knew it, and no way there until it comes back.
    bool drifted{ false };
};

// A link between two systems (for drawing on the star map).
struct SystemLink
{
    SystemLink() = default;
    SystemLink(std::string from, std::string to) : a(std::move(from)), b(std::move(to)) {}

    std::string a;
    std::string b;
    // Empty for a gate. "hole" for a roaming wormhole a pilot has found (#342 S9), "mouth"
    // for an unstable mouth not yet anchored (#342 S4, #432): either closes at `closesAt`
    // (world time) and lets through ships of at most `maxMass`.
    std::string kind;
    double      closesAt = 0.0;
    float       maxMass = 0.0f;
};

// Galaxy index: the list of systems, their links, and the starting system.
struct Universe
{
    std::vector<SystemInfo> systems;
    std::vector<SystemLink> links;
    std::string             startId;
};

// Reads the galaxy index from JSON at path. On error — an empty Universe.
Universe LoadUniverse(const std::string& path);

// Reads a star system from JSON at path and builds its entities.
// On error (file not found / malformed JSON) returns an empty list.
std::vector<std::unique_ptr<Entity>> LoadSystem(const std::string& path);

// Builds entities from already-parsed JSON (for the editor — rebuild after
// in-memory edits). Entity order matches the order in the JSON.
std::vector<std::unique_ptr<Entity>> BuildSystem(const nlohmann::json& data);

// Names every planet after its system and its place from the star out: "Helios Core I" is the
// innermost (#259). A rule rather than data: a generated system's planets are named the
// moment it is, and naming the system names them again. Ties in orbit keep the file's order.
void NamePlanets(std::vector<std::unique_ptr<Entity>>& entities, const std::string& systemName);

// The numeral a planet's place is written in: 1 -> "I", 14 -> "XIV". Empty below 1.
std::string RomanNumeral(int n);

// The other direction, for one object: the element BuildSystem would build it from, and the
// array of a system document it belongs in ("stations", "asteroidFields", ...). A save keeps
// an object a player added this way (#38), in the format the data is written in, so loading
// it is BuildSystem again rather than a second reader. Only what the document format
// describes is written -- state such as a searched wreck is the caller's. A kind the format
// cannot add one at a time (a star, a planet, a gate, a ship) gives an empty `array`.
nlohmann::json DescribeObject(const Entity& e, std::string& array);
}  // namespace WorldLoader
