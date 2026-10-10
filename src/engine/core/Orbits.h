#pragma once

#include "entities/Entity.h"
#include <memory>
#include <vector>

// Moves a system's planets and their satellites to where they are at a world time (#210).
//
// Everything here is a function of the clock and the data, never of how many ticks ran:
// the server calls it every tick for docking, mining and salvage, and the editor calls it
// to preview, and both get the same picture for the same time.
namespace Orbits
{
// Where a satellite is at `time`, around a planet standing at `planet`.
Vector2 SatellitePosition(const Orbit& o, Vector2 planet, double time);

// Planets first, in file order, then everything that orbits one of them. A satellite
// naming a planet the system does not have stays where it is.
void Place(std::vector<std::unique_ptr<Entity>>& entities, double time);
}  // namespace Orbits
