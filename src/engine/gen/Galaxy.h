#pragma once

#include <nlohmann/json_fwd.hpp>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// The galaxy's skeleton (#342): the places a region may one day occupy, and which of them
// may be joined. It is generated whole from the world seed and costs nothing -- a point on
// the map and a list of neighbours per slot -- while what is *in* a region is made only when
// that region opens, from (seed, slot) alone (Region.h).
//
// Slots lie on a hexagonal lattice, numbered in rings outward from slot 0. A slot's place,
// its kind and its links to lower-numbered slots depend on the seed and the slot and on
// nothing else, so raising the cap only adds outer slots and never moves one that exists.
//
// The generator's rules hold here as in Region.cpp (Rng.h says why): randomness only from
// Gen::Rng keyed by what is generated, no sin/cos/exp, no unordered containers.
namespace Gen
{

// What a slot is for. A centre is a civilisation's core region (S2): high security,
// stations of every kind, an owner. Everything else is frontier until the tide opens it.
// Regions an admin wove in (S7) will take slots of a kind of their own.
enum class SlotKind
{
    Frontier,
    Centre,
};

// The version of the skeleton's rules (#342 S3), apart from the regions'. A region opened is
// sealed -- kept as it was made -- so changing the regions' rules (GENERATOR_VERSION) no
// longer changes a live world; the skeleton is never sealed, so changing where slots are or
// which are centres still does, and a world saved under other skeleton rules is refused.
// Bump it whenever SkeletonJson changes for a given seed -- the golden test will say so.
inline constexpr int SKELETON_VERSION = 1;

// How many civilisation centres a world has (#342): an admin chooses 1 to 6, 3 unless told.
inline constexpr int DEFAULT_CENTRES = 3;
inline constexpr int MAX_CENTRES = 6;
// Centres are this many slots apart on the lattice at least, so the frontiers grow toward
// each other for a while before two civilisations meet.
inline constexpr int CENTRE_SPACING = 4;
// The skeleton a server lays out until the admin's cap exists (S3): enough for six centres
// and the rings around them.
inline constexpr int DEFAULT_REGION_CAP = 100;

const char* SlotKindName(SlotKind kind);

// Where an admin pack (#342 S7, Pack.h) was woven into a world, as world.json keeps it:
// the pack, what its files hashed to then, and the slot -- and, for a system pack, the
// system -- it took.
struct Woven
{
    std::string id;
    std::string kind;  // "region" or "system"
    uint64_t    hash = 0;
    int         slot = -1;
    std::string system;  // a system pack's, once its region has been made
};

struct Slot
{
    int              index = 0;
    int              ring = 0;                // lattice rings out from slot 0
    int              q = 0, r = 0;            // axial lattice coordinates
    double           mapX = 0.0, mapY = 0.0;  // on the galaxy map, in its units
    SlotKind         kind = SlotKind::Frontier;
    int              inward = -1;  // the neighbour one ring in that it always links to; -1 for 0
    std::vector<int> neighbours;   // linked slots, ascending
    // Lattice steps to the nearest centre: 0 for a centre, 1 beside one. Security falls and
    // richness grows with it. Counted on the lattice, not along links, so it depends on the
    // slot alone and not on how big the cap made the skeleton. 1 in a world without centres.
    int hops = 1;
    // A centre's major faction (an id from factions.json); empty for anything else.
    std::string owner;
};

// What a world's galaxy is made from. Everything in it is saved with the world, because
// each of them changes what the seed makes.
struct WorldParams
{
    uint64_t seed = 1;
    int      cap = DEFAULT_REGION_CAP;  // slots in the skeleton
    int      centres = 0;               // civilisation centres, 0 .. MAX_CENTRES
    // The major factions, in the order the seed hands them out to centres; one may hold
    // several centres when there are fewer of them. Pirates are never here: they have no
    // centre and are born on the frontier.
    std::vector<std::string> owners;
    // Who the frontier's lawless are and who owns what nobody owns -- the registry's
    // `pirates` and `unaffiliated` roles, as ids.
    std::string pirates = "Pirates";
    std::string unaffiliated = "Independent";
};

struct GalaxySkeleton
{
    uint64_t                         seed = 1;
    std::vector<Slot>                slots;    // by index, 0 .. cap-1
    std::vector<std::pair<int, int>> links;    // (a, b) with a < b, sorted
    std::vector<int>                 centres;  // centre slots, in the order they were placed
    std::string                      pirates = "Pirates";           // WorldParams'
    std::string                      unaffiliated = "Independent";  // WorldParams'
};

// The skeleton of `cap` slots (at least one), without centres. Every slot is linked to slot
// 0 through its `inward` neighbours, so whatever opens, the galaxy stays one piece.
GalaxySkeleton GenerateGalaxySkeleton(uint64_t seed, int cap);
// ...with the world's centres placed and owned. A centre the cap has no room for is left
// out rather than moved: where a centre is depends on the seed and the count, never the cap.
GalaxySkeleton GenerateGalaxySkeleton(const WorldParams& params);

// Where `count` centres go (#342 S2): on one ring of the lattice, evenly round it from a
// corner the seed turns, on the smallest ring that keeps every two CENTRE_SPACING apart.
// One centre is slot 0. In placement order.
std::vector<int> CentreSlots(uint64_t seed, int count);

// Steps between two slots' cells on the lattice.
int LatticeDistance(int a, int b);

// The regions a new world opens with: every centre, then one frontier slot beside each, so
// a player who starts anywhere has somewhere to go. `from` is the centre a frontier opens
// from (its mouth is in that centre's edge), -1 for a centre.
struct Opening
{
    int slot = 0;
    int from = -1;
};
std::vector<Opening> InitialOpenings(const GalaxySkeleton& skeleton);

// The skeleton as JSON: what `econserver galaxydump` prints and the golden test hashes.
nlohmann::json SkeletonJson(const GalaxySkeleton& skeleton);

// Where a slot's map point is, independent of the cap -- a region placed from a neighbour
// faces it without generating the skeleton around it.
void SlotMapPoint(uint64_t seed, int slot, double& x, double& y);

}  // namespace Gen
