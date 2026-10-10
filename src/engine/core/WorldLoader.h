#pragma once

#include "entities/Entity.h"
#include "raylib.h"
#include <nlohmann/json_fwd.hpp>
#include <vector>
#include <memory>
#include <string>

// Loading the game world from JSON files.
namespace WorldLoader
{
// A system's record in the galaxy index (data/universe.json).
struct SystemInfo
{
    std::string id;               // short identifier (= a gate's destination)
    std::string name;             // display name
    std::string file;             // file name under data/systems/
    Vector2     mapPos;           // position on the star map
    float       security = 0.5f;  // security level 0..1 (1 = safe)
    std::string owner;            // owning faction (id); empty — unclaimed system
    // Whether anybody has been there (#144). An uncharted system is known only to exist,
    // because a gate from a charted one leads to it: where it is on the map and what it
    // is designated, never its security, owner or contents.
    bool charted = true;
    // A generated system's designation, when it has been given a name (#145): "W-3.2"
    // stays the way to tell two people mean the same place. Empty otherwise.
    std::string designation;
    std::string discoverer;  // who got there first, if anybody has
};

// A link between two systems (for drawing on the star map).
struct SystemLink
{
    std::string a;
    std::string b;
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
