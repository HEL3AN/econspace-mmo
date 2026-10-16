#pragma once

#include "gen/Galaxy.h"
#include "gen/Region.h"
#include <nlohmann/json.hpp>

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

// Admin packs (#342 S7): hand-made content woven into a generated world. A pack is a
// directory, data/packs/<id>/, holding systems in the format the loaders already read and
// one file that says how they are woven in:
//
//   { "schema": 1, "id": "helios", "name": "Helios", "kind": "region",
//     "systems": [ { "id": "core", "file": "core.json", "map": [0, 0],
//                    "name": "Helios Core", "security": 1.0, "owner": "TradersGuild" } ],
//     "links": [ ["core", "reach"] ],
//     "factions": [ { factions.json's entry } ], "relations": [ ["helios-a", "Pirates", "War"] ],
//     "start": ["core"],
//     "weave": { "place": "random", "links": { "count": [1, 2], "kind": "static" } } }
//
// A "region" pack takes a free slot of the skeleton beside an open region; its systems are
// renamed for the slot (r<slot>-<depth>-<k>, depth counted along its links from its first
// system) and placed round the slot's point on the map, and admin links join it to systems
// of the open regions next to it. A "system" pack is one system put in place of a generated
// one -- a pin's "replace" (Pins.h) that comes with a name, an owner and a start. Weaving
// happens once, when the world is made; what it made is sealed with the regions, and a pack
// that changes later is named, not woven again (documents/world_generation_v2.md).
//
// Picks follow the generator's rules (Rng.h): keyed by the world's seed and the pack's id,
// never by the order things happen in, so the same packs make the same world anywhere.
namespace Gen
{

inline constexpr int PACK_SCHEMA = 1;
// The limits a pack is held to.
inline constexpr int    PACK_MAX_SYSTEMS = 40;
inline constexpr int    PACK_MAX_LINKS = 4;      // admin links out of one region pack
inline constexpr double PACK_MAP_REACH = 900.0;  // a system's "map" from the pack's middle

struct PackSystem
{
    std::string    id;    // the pack's own name for it; renamed when woven
    std::string    file;  // beside pack.json
    std::string    name;  // what a player reads; empty: designated when woven (S8)
    double         mapX = 0.0, mapY = 0.0;
    double         security = 0.5;
    std::string    owner;  // a faction id, or empty
    nlohmann::json document;
};

struct Pack
{
    std::string                                      id;
    std::string                                      name;
    std::string                                      kind = "region";  // or "system"
    std::string                                      dir;              // where it was read from
    std::vector<PackSystem>                          systems;
    std::vector<std::pair<std::string, std::string>> links;  // pack ids
    nlohmann::json                                   factions = nlohmann::json::array();
    nlohmann::json                                   relations = nlohmann::json::array();
    std::vector<std::string>                         starts;  // pack ids
    // weave.place: "random", "near-centre", "depth" (depthMin..depthMax lattice steps from
    // a centre), "slot" (exactly `slot`), or -- a system pack only -- "system" (`system`).
    std::string place = "random";
    int         depthMin = 0, depthMax = 0;
    int         slot = -1;
    std::string system;
    int         linksMin = 1, linksMax = 1;
    std::string linkKind = "static";
    // What it was read from, hashed: pack.json and every system file, each as parsed, so a
    // file reformatted is the same pack and a value changed is not.
    uint64_t hash = 0;
};

// The hash as it is written and compared: sixteen hex digits.
std::string PackHashText(uint64_t hash);

// Reads and checks the pack in `dir`. Every problem is one line in `problems`, naming the
// pack, the file and the field; the pack is usable only when there are none. Faction ids
// are checked against the registry and the pack's own factions, archetypes against theirs,
// so load both first.
bool LoadPack(const std::string& dir, Pack& out, std::vector<std::string>& problems);

// Every pack under `packsDir` (each subdirectory with a pack.json), sorted by id. A
// directory that does not exist has none. Two packs with one id is a problem.
std::vector<Pack> LoadPacks(const std::string& packsDir, std::vector<std::string>& problems);

// The station a pilot starting in this system document docks at: its first dockable one,
// built as the server builds it (archetypes loaded). Empty when it has none.
std::string DockingStation(const nlohmann::json& document);

// The slot a region pack takes: a free slot (not open, not a centre, not in `taken`) linked
// to an open one and matching weave.place, picked by the seed and the pack's id. -1, and a
// line in `problems`, when there is none.
int PlaceRegionPack(const Pack& pack, const GalaxySkeleton& skeleton, const std::set<int>& open,
                    const std::set<int>& taken, std::vector<std::string>& problems);

// The slot a system pack goes into: the frontier slots `candidates` offers (the open ones a
// new world began with), filtered by weave.place; `slot` and `system` name one outright. -1
// and a problem when nothing matches.
int PlaceSystemPack(const Pack& pack, const GalaxySkeleton& skeleton,
                    const std::vector<int>& candidates, std::vector<std::string>& problems);

// A region pack made into the region of `slot`. `neighbours` are the open regions linked to
// the slot, with their documents (the admin links' gates are placed in them clear of their
// planets); the links are drawn by the seed and the pack's id. The gates on the far side go
// into `crossings` -- they belong to this region, kept beside the systems they stand in.
Region WeaveRegionPack(const Pack& pack, const GalaxySkeleton& skeleton, int slot,
                       const std::vector<Region>& neighbours, std::vector<std::string>& problems);

// The system a system pack replaces in `region`: `wanted` when it names one, otherwise one
// the seed and the pack's id pick among those that are neither the way in nor anyone's
// station. Empty when there is none.
std::string SystemPackTarget(const Pack& pack, const Region& region, uint64_t seed,
                             const std::string& wanted);

// Puts a system pack's system in place of `system` of `region`: its document (gates kept,
// as a pin's replace keeps them), its name, security and owner, and its start. False, with
// the reason in `problems`, when it cannot.
bool ApplySystemPack(const Pack& pack, Region& region, const std::string& system,
                     std::vector<std::string>& problems);

// Puts a region's pack factions into the registry, with their relations. Call, like
// RegisterNatives, before its systems are built.
void RegisterPackFactions(const nlohmann::json& factions, const nlohmann::json& relations);

// ---- Writing a pack (#342 S8): the editor's side ----------------------------------------
//
// What a pack directory holds, before it is written: pack.json and each system document by
// its file name. Written as LoadPack reads it -- the editor writes exactly the format the
// runtime reads.
struct PackFiles
{
    nlohmann::json                        pack = nlohmann::json::object();
    std::map<std::string, nlohmann::json> documents;  // file name beside pack.json -> document
};

// True when `id` may name a pack (and so its directory): lower-case letters, digits, '-'
// and '_', at most 40 of them.
bool PackIdValid(const std::string& id);

// A region as a pack of id `id`: its systems under ids of the pack's own (the slot taken off,
// "r14-2-3" -> "s2-3"), the way in first, the map moved round the pack's middle and drawn in
// to PACK_MAP_REACH if it reaches further; its links and gates among its own systems, and
// nothing that leads out of it -- the wormhole home and any admin link are the weaving's to
// make. Its natives become a faction of the pack's own ("<id>-natives"), with their stance
// to the majors and the pirates; a centre's heart and any start it had become its starts.
// `notes` (may be null) gets a line for each thing left behind.
PackFiles RegionAsPack(const Region& region, const std::string& id, const std::string& name,
                       std::vector<std::string>* notes = nullptr);

// A region pack begun from nothing: one system, "core", with a star, two planets and a
// station a new pilot can dock at, so it reads and weaves as it is -- the start of
// something, to be grown in the editor.
PackFiles NewRegionPack(const std::string& id, const std::string& name);

// Writes `files` into `dir` (made if missing): pack.json and every document. A directory
// that already holds a pack.json is refused unless `overwrite`. False, with `error`, when it
// cannot.
bool WritePack(const std::string& dir, const PackFiles& files, bool overwrite, std::string& error);

}  // namespace Gen
