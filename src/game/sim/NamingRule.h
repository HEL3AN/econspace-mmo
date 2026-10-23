#pragma once

#include "core/WorldLoader.h"

#include <string>

namespace Names
{
// Why `pilot` may not name this system (#513, #431 N1-N2), in words for whoever asked; empty
// if they may. The one rule set: the server refuses with it, and the map and the agent show it
// before asking, over what the galaxy index tells. What a name must be (N3) is `NameProblem`
// on the server, not this.
std::string NamingRefusal(const WorldLoader::SystemInfo& si, const std::string& pilot);
}  // namespace Names
