#pragma once

// The resident memory of this process in MiB, or a negative number when the platform will
// not say. For benchmarks (#295); nothing in the game depends on it.
double ResidentMegabytes();
