#pragma once

#include <cstdint>
#include <vector>

// `econserver macrobench` (#295): for each region size, builds a galaxy whose region beyond
// the wormhole has that many systems, runs the host's world step for `seconds` of simulated
// time with nobody connected, and prints where the time went, as a table. Returns the
// process exit code.
int MacroBench(const std::vector<int>& regionSizes, double seconds, uint64_t seed);
