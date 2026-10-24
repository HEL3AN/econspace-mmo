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

// `econserver netbench` (#350): the per-player side. For each crowd size, that many pilots fly
// circles in one system -- a start, with its NPCs -- and the host's work for them is timed as
// the host does it: every pilot's step each tick, the world's step, and twenty times a second
// a snapshot built and encoded for each of them against what they acknowledged. Prints a
// table of where the time goes per tick, and the bytes a snapshot costs.
int NetBench(const std::vector<int>& crowds, double seconds, uint64_t seed);

// `econserver fleetbench` (#464, slice 4): the long run of a world whose factions buy their
// ships. For each seed, a centre, its frontier and a region of `regionSystems` run cold for
// `days` world days, one coarse pass at a time, and every `everyHours` the table prints, per
// faction that holds anything, its ships in all (presence), its holdings, what they keep, and
// its treasury -- the curves the slice's PR shows. Fails (exit 1) where a faction that still
// holds systems has let its ships fall to nothing: the poor faction emptying the map the
// owner asked to guard against. With `shockHours`, at that hour every holding loses 70% of
// its ships at once -- a war everywhere -- and the table shows what comes back, and how fast.
int FleetBench(const std::vector<uint64_t>& seeds, double days, int regionSystems,
               double everyHours, double shockHours);
