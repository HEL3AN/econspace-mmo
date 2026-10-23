#include "sim/NamingRule.h"

#include "core/Faction.h"

namespace Names
{
std::string NamingRefusal(const WorldLoader::SystemInfo& si, const std::string& pilot)
{
    FactionId by;
    if (si.nativeNamed)  // theirs, revealed at first contact (N1)
        return "its people call it " + si.name;
    if (!si.designation.empty())
        return si.name + " already has its name";
    if (si.discoverer.empty())
    {
        if (!si.chartedBy.empty())  // a faction came first (N2)
            return (Factions::Find(si.chartedBy, by) ? FactionName(by) : std::string("a faction")) +
                   " charted it first";
        if (si.charted && si.knownBy.empty())  // on the public charts: reached by somebody
            return "it was on the charts before anyone here; its name is the world's";
        return "nobody has found it yet; its discoverer may name it";
    }
    if (si.discoverer != pilot)
        return "only " + si.discoverer + ", who got here first, may name it";
    return std::string();
}
}  // namespace Names
