#include "core/Route.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <queue>
#include <utility>

namespace Route
{
const char* PreferenceName(Preference p)
{
    switch (p)
    {
        case Preference::Safer: return "safer";
        case Preference::LessSecure: return "less_secure";
        case Preference::Shortest: break;
    }
    return "shortest";
}

bool PreferenceFromName(const std::string& name, Preference& out)
{
    for (int i = 0; i <= PREFERENCE_MAX; ++i)
        if (name == PreferenceName((Preference)i))
        {
            out = (Preference)i;
            return true;
        }
    return false;
}

bool HighSec(float security)
{
    // Tenths, as the map prints it: lround, never a float compare at the boundary.
    return std::lround((double)security * 10.0) >= 5;
}

double HopCost(Preference p, float security)
{
    switch (p)
    {
        case Preference::Safer: return HighSec(security) ? 1.0 : HOP_PENALTY;
        case Preference::LessSecure: return HighSec(security) ? HOP_PENALTY : 1.0;
        case Preference::Shortest: break;
    }
    return 1.0;
}

bool Passable(const WorldLoader::SystemLink& link, double mass, double now)
{
    return (link.closesAt <= 0.0 || link.closesAt > now) &&
           (link.maxMass <= 0.0f || mass <= (double)link.maxMass);
}

Result Find(const WorldLoader::Universe& known, const std::string& from, const std::string& to,
            double mass, double now, const std::function<double(const std::string&)>& cost)
{
    std::map<std::string, std::vector<const WorldLoader::SystemLink*>> edges;
    for (const auto& system : known.systems)
        edges[system.id];
    if (edges.count(from) == 0)
        return { {}, "Your current system is not on the known map." };
    if (edges.count(to) == 0)
        return { {}, to + " is not on the known map." };
    if (from == to)
        return { { from }, {} };
    for (const auto& link : known.links)
    {
        auto a = edges.find(link.a);
        auto b = edges.find(link.b);
        if (a != edges.end() && b != edges.end())
        {
            a->second.push_back(&link);
            b->second.push_back(&link);
        }
    }

    // Ties by id: a node is settled in (distance, id) order, so of two equal ways the one
    // through the lower id is found first and kept -- whatever order the links came in.
    using Entry = std::pair<double, std::string>;
    std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> queue;
    std::map<std::string, double>                                       distance{ { from, 0.0 } };
    std::map<std::string, std::string>                                  previous;
    queue.push({ 0.0, from });
    bool expired = false, heavy = false;
    while (!queue.empty())
    {
        const Entry top = queue.top();
        queue.pop();
        const double       d = top.first;
        const std::string& at = top.second;
        if (d != distance.at(at))
            continue;
        if (at == to)
        {
            Result result;
            for (std::string node = to;; node = previous.at(node))
            {
                result.path.push_back(node);
                if (node == from)
                    break;
            }
            std::reverse(result.path.begin(), result.path.end());
            return result;
        }
        for (const auto* link : edges.at(at))
        {
            if (!Passable(*link, mass, now))
            {
                expired = expired || (link->closesAt > 0.0 && link->closesAt <= now);
                heavy = heavy || (link->maxMass > 0.0f && mass > (double)link->maxMass);
                continue;
            }
            const std::string& next = link->a == at ? link->b : link->a;
            const double       via = d + (cost ? std::max(1.0, cost(next)) : 1.0);
            auto               best = distance.find(next);
            if (best != distance.end() && best->second <= via)
                continue;
            distance[next] = via;
            previous[next] = at;
            queue.push({ via, next });
        }
    }
    std::string reason = "No route to " + to + " through known gates.";
    if (expired)
        reason += " A wormhole on the way has closed.";
    if (heavy)
        reason += " Your ship is too heavy for a wormhole on the way.";
    return { {}, reason };
}

Result Find(const WorldLoader::Universe& known, const std::string& from, const std::string& to,
            double mass, double now, Preference preference)
{
    if (preference == Preference::Shortest)
        return Find(known, from, to, mass, now);
    std::map<std::string, float> security;
    for (const auto& system : known.systems)
        security[system.id] = system.security;
    return Find(known, from, to, mass, now,
                [&](const std::string& id)
                {
                    auto it = security.find(id);
                    return HopCost(preference, it != security.end() ? it->second : 0.0f);
                });
}

Result FindVia(const WorldLoader::Universe& known, const std::string& from,
               const std::vector<std::string>& stops, double mass, double now,
               Preference preference)
{
    Result      whole;
    std::string at = from;
    whole.path.push_back(from);
    for (const std::string& stop : stops)
    {
        Result leg = Find(known, at, stop, mass, now, preference);
        if (leg.path.empty())
            return { {}, leg.reason };
        whole.path.insert(whole.path.end(), leg.path.begin() + 1, leg.path.end());
        at = stop;
    }
    return whole;
}
}  // namespace Route
