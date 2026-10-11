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
    6;  // 6: keys and ids by skeleton slot (#342); 5: the leviathan dwarfs a station

struct RegionParams
{
    uint64_t seed = 1;  // the world's seed
    // The skeleton slot the region fills (#342, Galaxy.h). Every stream the region draws
    // from is keyed by (seed, slot, ...) and its ids carry the slot, so what is in a region
    // does not depend on which others are open or in what order they opened.
    int         slot = 0;
    int         systems = 18;  // how many systems the region has
    std::string homeId;        // the system the wormhole opens from
    Vector2     homeMap = { 0.0f, 0.0f };
    // Which way the region grows from home on the galaxy map. Zero: the bearing furthest
    // from `knownMap`, among eight the seed orders -- how a region off a hand-written system
    // finds room. A caller keeps the result independent of opening order by passing only
    // systems that never change (the hand-written ones), or a bearing from the skeleton.
    Vector2              bearing = { 0.0f, 0.0f };
    std::vector<Vector2> knownMap;
    // The home system's document, so the wormhole is not placed in a planet's path.
    // May be null: then only the system's edge is respected.
    const nlohmann::json* homeSystem = nullptr;
};

struct Region
{
    int slot = 0;  // the skeleton slot it was made for; a pin names it (Pins.h)
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

// A region as one self-contained document, and back (#342): its slot, index entries, links,
// system documents and the wormhole's mouth -- everything the server builds it from, so an
// opened region can be kept as it was made (regions/<slot>.json, S3) instead of being
// remade by whatever rules a later build has. False, and `out` untouched, for a document
// that is not one.
nlohmann::json RegionToJson(const Region& region);
bool           RegionFromJson(const nlohmann::json& doc, Region& out);

// A generated system's id: "r<slot>-<depth>-<k>" (#342), unique in the whole galaxy.
std::string SystemId(int slot, int depth, int k);

// The ring a system is in: 1 is through the wormhole, and everything gets worse with it
// (#143). Read back from an id; 0 for a system that was not generated.
int DepthOf(const std::string& id);

// The skeleton slot a generated system belongs to, read back from its id; -1 for a system
// that was not generated.
int SlotOf(const std::string& id);

// How far out from a planet its satellites may go (#210): a planet's path is wider than the
// planet, because it takes what orbits it along. The server puts a faction's orbital outpost
// (#318) under the same limit as the generator's moons.
inline double MoonZone(double planetSize)
{
    return planetSize * 1.5 + 12000.0;
}

}  // namespace Gen
