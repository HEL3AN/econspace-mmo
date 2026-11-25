#pragma once

#include "render/Silhouette.h"
#include <vector>

namespace Render
{
namespace Built3D
{

// The structure a composition implies and its data does not spell out (#618).
//
// A trade hub is written as a hexagon, three arms with pads, and a ring. Seen as a solid that
// is not enough to be a station: an arm is a pipe with nothing on it, a ring has nothing to
// hold it, a pad is a ball. Nothing here is placed by hand and nothing knows an archetype.
// Each rule reads what a piece *is* -- a long member, a large plate, a ring, a pad -- and adds
// what such a thing has: a truss under a member and collars along it, stepped plating and
// buttresses on a plate, ribs, habitat blocks of several sizes and lamps on a ring, spokes from
// the hub to a ring that would otherwise float, radiator fins on those spokes, masts and a dish
// on a roof, tanks and lamps round a pad.
//
// The choices come from the object's seed and each source piece's index, through a small
// integer hash, so two clients and two compilers agree and nothing depends on the order
// objects were drawn in (#140). More is added as the object is bigger on screen: a speck keeps
// its silhouette, and the one that fills the screen gets its rivets.
//
// Pure geometry: it takes pieces and returns pieces, no GPU, tested.

// How much to add, from how big the object is on screen.
//   0  nothing (the object is a few dozen pixels)
//   1  members, plates, rings, spokes
//   2  everything, including the small modules, masts and lamps
int DetailLevel(float pixelRadius);

// `composed` is what `Compose` returned for an object of radius `objectSize` at the origin.
// Returns those pieces with the generated ones merged in, in drawing order. `seed` is the
// object's (its id).
std::vector<Piece> Embellish(const std::vector<Piece>& composed, float objectSize, int seed,
                             int level);

}  // namespace Built3D
}  // namespace Render
