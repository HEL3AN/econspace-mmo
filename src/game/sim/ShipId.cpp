#include "entities/ShipType.h"

#include "core/Archetype.h"

// Ship ids (#356): in netproto, because the wire names ships by them and the fuzzers link nothing
// above it. A key's hull is its place in the hangar list, which is also the catalog's order.
std::string ShipIdOf(int key)
{
    const std::vector<Ships::HangarEntry>& hangar = Archetypes::ShipCatalogue().hangar;
    const int                              hull = ShipHull(key);
    if (hull < 0 || hull >= (int)hangar.size())
        return std::string();
    std::string id = hangar[(size_t)hull].design;
    if (key / SHIP_STRIDE > 0)
        id += "#" + std::to_string(key / SHIP_STRIDE);
    return id;
}

bool ShipKeyFromId(const std::string& id, int& key)
{
    std::string design = id;
    int         copy = 0;
    if (const size_t cut = id.rfind('#'); cut != std::string::npos)
    {
        design = id.substr(0, cut);
        const std::string n = id.substr(cut + 1);
        if (n.empty() || n.size() > 3 || n.find_first_not_of("0123456789") != std::string::npos)
            return false;
        copy = std::stoi(n);
        if (copy < 1)
            return false;
    }
    const std::vector<Ships::HangarEntry>& hangar = Archetypes::ShipCatalogue().hangar;
    for (size_t i = 0; i < hangar.size(); i++)
        if (hangar[i].design == design)
        {
            key = ShipKey((int)i, copy);
            return true;
        }
    return false;
}
