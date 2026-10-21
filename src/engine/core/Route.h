#pragma once

#include "core/WorldLoader.h"

#include <functional>
#include <string>
#include <vector>

// The one route planner (#432): the same search for a map preview on the client and a
// route order on the server, so what a player is shown is the way the ship flies.
//
// The caller supplies the graph -- for a pilot, the systems and links that pilot knows
// (Simulation::KnownUniverseFor). Positions on the map never decide whether a jump exists.
// The search is Dijkstra with every tie broken by system id, so two peers that hold the
// same index in a different order find the same path, and the costs are small integers,
// so no two compilers can round them apart.
namespace Route
{
// How a route weighs a hop, as EVE's autopilot does. Security is the system's own (its
// base, the number in the index), never the live drift: both ends must weigh it the same.
enum class Preference
{
    Shortest = 0,    // fewest jumps
    Safer = 1,       // keep to security 0.5 and above where any way does
    LessSecure = 2,  // keep below 0.5 where any way does
};
inline constexpr int PREFERENCE_MAX = (int)Preference::LessSecure;

// "shortest", "safer", "less_secure"; and back. Unknown text is false.
const char* PreferenceName(Preference p);
bool        PreferenceFromName(const std::string& name, Preference& out);

// What entering a system of this security costs under a preference: 1 for a hop the
// preference likes, HOP_PENALTY for one it does not. Rounded to the tenth a player is shown:
// a system the map prints as 0.5 is high security, one it prints as 0.4 is not.
inline constexpr double HOP_PENALTY = 50.0;
double                  HopCost(Preference p, float security);
bool                    HighSec(float security);

struct Result
{
    std::vector<std::string> path;    // from .. to inclusive; empty -- no route
    std::string              reason;  // why not, when empty
};

// Whether a link lets a ship of `mass` through at world time `now`: a gate always, a mouth
// or a roaming hole until it closes and only up to its mass.
bool Passable(const WorldLoader::SystemLink& link, double mass, double now);

// One leg, weighed by `cost(system entered)` (at least 1 per hop).
Result Find(const WorldLoader::Universe& known, const std::string& from, const std::string& to,
            double mass, double now, const std::function<double(const std::string&)>& cost = {});
// One leg under a preference, read from the systems' security in `known`.
Result Find(const WorldLoader::Universe& known, const std::string& from, const std::string& to,
            double mass, double now, Preference preference);
// Through `stops` in order (waypoints, then the destination): each leg planned from where
// the last one ended, and the legs joined. A stop the route is already at is passed.
Result FindVia(const WorldLoader::Universe& known, const std::string& from,
               const std::vector<std::string>& stops, double mass, double now,
               Preference preference);
}  // namespace Route
