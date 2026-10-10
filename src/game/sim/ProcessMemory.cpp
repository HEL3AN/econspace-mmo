// How much memory this process holds, for `econserver macrobench` (#295).
//
// A file of its own because it needs <windows.h>, whose names collide with raylib's, and
// everything else in the server includes raylib.

#include "sim/ProcessMemory.h"

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#else
#include <cstdio>
#include <unistd.h>
#endif

double ResidentMegabytes()
{
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS pmc{};
    // K32 by name: it lives in kernel32 on every Windows we build for, so no psapi to link.
    if (!K32GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)))
        return -1.0;
    return (double)pmc.WorkingSetSize / (1024.0 * 1024.0);
#else
    FILE* f = std::fopen("/proc/self/statm", "r");
    if (f == nullptr)
        return -1.0;
    long      size = 0, resident = 0;
    const int read = std::fscanf(f, "%ld %ld", &size, &resident);
    std::fclose(f);
    if (read != 2)
        return -1.0;
    return (double)resident * (double)sysconf(_SC_PAGESIZE) / (1024.0 * 1024.0);
#endif
}
