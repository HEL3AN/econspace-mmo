#pragma once

#include <cstdint>
#include <vector>

// `econserver macrobench` (#295): for each region size, builds a galaxy whose region beyond
// the wormhole has that many systems, runs the host's world step for `seconds` of simulated
// time, and prints where the time went, as a table. Returns the process exit code.
//
// Nobody is connected unless `players` says otherwise: that many sessions, each in a system
// of its own spread across the galaxy, standing still. With `allHot`, every system is kept
// hot as before cold systems (#295), for the comparison.
int MacroBench(const std::vector<int>& regionSizes, double seconds, uint64_t seed, bool allHot,
               int players);
