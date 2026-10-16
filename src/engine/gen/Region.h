#pragma once

#include "raylib.h"
#include <nlohmann/json.hpp>

#include "core/Faction.h"
#include "gen/Galaxy.h"

#include <cstdint>
#include <functional>
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
    9;  // 9: a centre's station in a planet's path orbits it (#377);
        // 8: every frontier region has natives, with stations and markets (#342 S5);
        // 7: civilisation centres, and a frontier as rich and lawless as it is far from one
        // (#342 S2); 6: keys and ids by skeleton slot (#342); 5: the leviathan dwarfs a station

struct RegionParams
{
    uint64_t seed = 1;  // the world's seed
    // The skeleton slot the region fills (#342, Galaxy.h). Every stream the region draws
    // from is keyed by (seed, slot, ...) and its ids carry the slot, so what is in a region
    // does not depend on which others are open or in what order they opened.
    int slot = 0;
    // What the slot is (S2): a frontier region opened through a wormhole, or a civilisation
    // centre -- named, policed, built up and owned, with no home and no wormhole of its own.
    SlotKind    kind = SlotKind::Frontier;
    int         hops = 1;  // lattice steps to the nearest centre: a frontier's law and riches
    std::string owner;     // a centre's major faction
    // The registry's roles, by id (#355): who holds a lawless frontier system, and who owns a
    // shipyard that builds for anyone.
    std::string pirates = "Pirates";
    std::string unaffiliated = "Independent";
    int         systems = 18;  // how many systems a frontier region has (a centre picks its own)
    std::string homeId;        // the system the wormhole opens from; empty for a centre
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
    // The wormhole: a gate object to add to the home system's "gates". Empty, with an
    // empty `homeId`, for a centre, which no wormhole leads into.
    nlohmann::json wormhole = nlohmann::json::object();
    std::string    homeId;
    // A centre's heart: the capital, where its trade hub, shipyard and fleet are, and where
    // a player may start. Empty for a frontier region.
    std::string heartId;
    // A frontier region's own minor faction (#342 S5): who lives there before anyone comes
    // through the mouth. {id, name, theme, color, lawful, temperament, relations {majors,
    // pirates}, home, systems}; empty for a centre, and for a region sealed before natives.
    // Its id is a hash of the region's key (seed, slot): what saves and the wire call it.
    nlohmann::json natives = nlohmann::json::object();
    // What an admin pack wove in (#342 S7, Pack.h); all empty for a generated region but
    // `starts` and `pack`, which a system pack put into one also sets.
    std::string              pack;    // the pack's id
    std::vector<std::string> starts;  // systems a player may start in, beside a centre's heart
    // The admin links' gates on the far side: [{ "home": a system of another region,
    // "gate": a gate object }], added to that system when it is built, like a wormhole's mouth.
    nlohmann::json crossings = nlohmann::json::array();
    // The pack's own factions (entries of factions.json) and their stances
    // ([[a, b, stance]]), registered whenever the region is hung in the galaxy.
    nlohmann::json factions = nlohmann::json::array();
    nlohmann::json relations = nlohmann::json::array();
};

// Puts a region's natives into the faction registry (Factions::Add) with their stance to
// the majors and the pirates, and returns them; one already there is returned unchanged. The
// unaffiliated faction for a region without natives. Call before the region's systems are
// built: their stations fly the natives' flag.
FactionId RegisterNatives(const nlohmann::json& natives);

Region GenerateRegion(const RegionParams& params);

// What a slot of the skeleton is generated with: its kind, its distance from a centre and
// its owner. The home the region opens from is the caller's (HomeFrom).
RegionParams SlotParams(const GalaxySkeleton& skeleton, int slot);

// The system of an open region a wormhole into `toward` (a point on the galaxy map) opens
// from: one of its deepest, the nearest to where it leads. Empty for a region with none.
std::string MouthSystem(const Region& from, double towardX, double towardY);

// Points `params` at `homeId` of the open region `from` for its wormhole: home's place on
// the map, its document (so the mouth misses its planets) and the bearing the skeleton
// gives from home's slot to the new one. `homeDoc` must outlive the call to GenerateRegion.
void HomeFrom(RegionParams& params, const GalaxySkeleton& skeleton, const Region& from,
              const std::string& homeId, const nlohmann::json& homeDoc);

// Every region a new world opens with (InitialOpenings), made in that order: centres, then
// a frontier off each. `pin` (may be empty) is applied to each as it is made, before the
// next opens from it -- the server's pins, so a mouth is placed in the system as pinned.
std::vector<Region> GenerateWorld(const GalaxySkeleton&               skeleton,
                                  const std::function<void(Region&)>& pin = nullptr);

// A region as one self-contained document, and back (#342): its slot, index entries, links,
// system documents and the wormhole's mouth -- everything the server builds it from, so an
// opened region can be kept as it was made (regions/<slot>.json, S3) instead of being
// remade by whatever rules a later build has. False, and `out` untouched, for a document
// that is not one.
nlohmann::json RegionToJson(const Region& region);
bool           RegionFromJson(const nlohmann::json& doc, Region& out);

// A sealed region's file (regions/<slot>.json, S3): { schema, seed, generator, region }. Read
// by the server and by the editor opening a saved world (S8).
inline constexpr int SEAL_SCHEMA = 1;
// The region a sealed file's text holds: false for one that is not, or is another world's --
// a world.json moved aside and its regions left behind is not this seed's, whatever its slot.
bool ReadSealedRegion(const std::string& text, uint64_t seed, Region& out);

// A gate's place at the edge of `system` (may be null), facing (dx, dy) -- the system it
// leads to on the map -- and out of its planets' paths: where a wormhole's mouth goes, and
// an admin link's gate (S7). Drawn from `key`. A position, [x, y].
nlohmann::json EdgeGatePos(uint64_t key, const nlohmann::json* system, double dx, double dy);

// A generated system's id: "r<slot>-<depth>-<k>" (#342), unique in the whole galaxy.
std::string SystemId(int slot, int depth, int k);

// What a player calls that system until somebody names it (#145): "W-2.3" in slot 0,
// "R14 W-2.3" anywhere else. A pack's system without a name is designated so (S8).
std::string SystemDesignation(int slot, int depth, int k);

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
