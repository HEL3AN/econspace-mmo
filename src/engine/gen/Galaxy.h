#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
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

// What a slot is for. Only frontier regions exist so far; civilisation centres (S2) and
// regions an admin wove in (S7) take slots of their own kinds.
enum class SlotKind
{
    Frontier,
};

const char* SlotKindName(SlotKind kind);

struct Slot
{
    int              index = 0;
    int              ring = 0;                // lattice rings out from slot 0
    int              q = 0, r = 0;            // axial lattice coordinates
    double           mapX = 0.0, mapY = 0.0;  // on the galaxy map, in its units
    SlotKind         kind = SlotKind::Frontier;
    int              inward = -1;  // the neighbour one ring in that it always links to; -1 for 0
    std::vector<int> neighbours;   // linked slots, ascending
};

struct GalaxySkeleton
{
    uint64_t                         seed = 1;
    std::vector<Slot>                slots;  // by index, 0 .. cap-1
    std::vector<std::pair<int, int>> links;  // (a, b) with a < b, sorted
};

// The skeleton of `cap` slots (at least one). Every slot is linked to slot 0 through its
// `inward` neighbours, so whatever opens, the galaxy stays one piece.
GalaxySkeleton GenerateGalaxySkeleton(uint64_t seed, int cap);

// The skeleton as JSON: what `econserver galaxydump` prints and the golden test hashes.
nlohmann::json SkeletonJson(const GalaxySkeleton& skeleton);

// Where a slot's map point is, independent of the cap -- a region placed from a neighbour
// faces it without generating the skeleton around it.
void SlotMapPoint(uint64_t seed, int slot, double& x, double& y);

}  // namespace Gen
