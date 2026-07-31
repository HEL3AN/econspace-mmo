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
    std::string id;            // short identifier (= a gate's destination)
    std::string name;          // display name
    std::string file;          // file name under data/systems/
    Vector2     mapPos;        // position on the star map
    float       security = 0.5f;  // security level 0..1 (1 = safe)
    std::string owner;         // owning faction (id); empty — unclaimed system
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
}
