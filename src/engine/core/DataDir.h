#pragma once

#include <string>

// Where the game's data/ folder is, decided once at startup (#399 P0-1).
//
// A binary used to find data/ through the source-tree path baked in at configure time, so
// a build copied to another machine could not start. Now every executable resolves it at
// runtime, in this order:
//   1. `--data <dir>` on the command line (removed from argv, so no mode sees it);
//   2. the ECONSPACE_DATA environment variable -- another checkout's data seen through
//      this build;
//   3. the source data/, when the executable still runs from the build tree that compiled
//      it and the build kept the dev path (ECON_DEV_DATA) -- what the editor writes is what
//      the game reads, with no rebuild in between;
//   4. data/ beside the executable, then data/ one level up (a bin/ beside data/);
//   5. the source data/ again, as a last resort for a dev build started from elsewhere.
// The choice is logged to stderr, never stdout: econagent's stdout is JSON-RPC.
namespace DataDir
{
// Resolves the folder for this process and remembers it. `who` prefixes the log line.
// Returns the folder with a trailing slash; empty when nothing was found, after saying so.
std::string Resolve(int& argc, char** argv, const char* who);

// The folder Resolve chose (trailing slash), or "data/" relative to the working directory
// before it was called -- the tests never call it and pass their own paths.
const std::string& Path();
}  // namespace DataDir
