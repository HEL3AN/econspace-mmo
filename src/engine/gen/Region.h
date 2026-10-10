#pragma once

#include "raylib.h"
#include <nlohmann/json.hpp>

#include <cstdint>
#include <map>
#include <string>
#include <vector>

// The region beyond the wormhole, generated from a seed (#140).
//
// The generator writes the documents the loaders already read: a fragment of the galaxy
// index in the shape of data/universe.json, and one system document per system in the
// shape of data/systems/*.json. Everything downstream -- WorldLoader::BuildSystem,
// materialization, the layout a client is sent -- is unchanged, determinism is testable
// as byte-identical JSON, and a hand-written system (#147) is simply a document put in
// place of a generated one.
//
// Rules for anything added here (Rng.h says why): randomness only from Gen::Rng keyed by
// what is being generated; no sin/cos/exp; no unordered containers.
namespace Gen
{

// The version of the rules. A world is saved with the version that made it, and a save
// from other rules is refused (#140): the same seed read by different rules is a different
// galaxy, and a player's structure would be standing in space that is no longer there.
// Bump it whenever the output for a given seed changes -- the golden test will say so.
inline constexpr int GENERATOR_VERSION =
    5;  // 5: the leviathan dwarfs a station; 4: placement with reasons (#146)

struct RegionParams
{
    uint64_t    seed = 1;
    int         systems = 18;  // how many systems the region has
    std::string homeId;        // the system the wormhole opens from
    Vector2     homeMap = { 0.0f, 0.0f };
    // Known systems already on the galaxy map: the region grows away from them rather
    // than on top of them.
    std::vector<Vector2> knownMap;
    // The home system's document, so the wormhole is not placed in a planet's path.
    // May be null: then only the system's edge is respected.
    const nlohmann::json* homeSystem = nullptr;
};

struct Region
{
    // Index entries in the shape of universe.json's "systems" (without "file") and
    // "links" -- including the link from home to the entry system.
    nlohmann::json systems = nlohmann::json::array();
    nlohmann::json links = nlohmann::json::array();
    // id -> system document, in the shape of data/systems/*.json.
    std::map<std::string, nlohmann::json> documents;
    std::string                           entryId;  // the system on the far side
    // The wormhole: a gate object to add to the home system's "gates".
    nlohmann::json wormhole;
};

Region GenerateRegion(const RegionParams& params);

// The ring a system is in: 1 is through the wormhole, and everything gets worse with it
// (#143). Read back from an id.
int DepthOf(const std::string& id);

// How far out from a planet its satellites may go (#210): a planet's path is wider than the
// planet, because it takes what orbits it along. The server puts a faction's orbital outpost
// (#318) under the same limit as the generator's moons.
inline double MoonZone(double planetSize)
{
    return planetSize * 1.5 + 12000.0;
}

}  // namespace Gen
