#include "sim/Compat.h"

#include "core/Version.h"
#include "sim/Auth.h"

#include <fstream>
#include <iterator>

namespace Compat
{

const std::vector<std::string>& SharedFiles()
{
    // archetypes.json reads ships.json beside it, and what an object can do (dock, trade,
    // refit) is its archetype's; goods are the market's order; blueprints, what can be built.
    static const std::vector<std::string> files = { "archetypes.json", "blueprints.json",
                                                    "factions.json", "goods.json", "ships.json" };
    return files;
}

std::map<std::string, std::string> DataHashes(const std::string& dataDir)
{
    std::map<std::string, std::string> out;
    for (const std::string& name : SharedFiles())
    {
        std::ifstream in(dataDir + name, std::ios::binary);
        if (!in.is_open())
        {
            out[name] = std::string();
            continue;
        }
        std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        std::string lf;
        lf.reserve(text.size());
        for (char c : text)
            if (c != '\r')
                lf.push_back(c);
        // Sixteen hex digits: this tells two honest copies apart, it is not a signature.
        out[name] = Auth::Sha256Hex(lf).substr(0, 16);
    }
    return out;
}

namespace
{
// "EconSpace 0.1.0 Anchor" for this build's own number, the bare number for any other.
std::string Named(const std::string& build)
{
    if (build.empty())
        return "an unknown build";
    return "EconSpace " + build +
           (build == Version::NUMBER ? std::string(" ") + Version::CODENAME : "");
}
}  // namespace

std::string Refusal(const std::string&                        serverBuild,
                    const std::map<std::string, std::string>& serverData,
                    const std::string&                        clientBuild,
                    const std::map<std::string, std::string>& clientData)
{
    if (clientBuild != serverBuild)
        return "This server runs " + Named(serverBuild) + " and your client is " +
               (clientBuild.empty() ? "an unknown build" : clientBuild) + ": get " +
               Named(serverBuild) + " from " + Version::RELEASES_URL;

    std::string differ;
    for (const std::string& name : SharedFiles())
    {
        const auto        s = serverData.find(name);
        const auto        c = clientData.find(name);
        const std::string ours = s == serverData.end() ? std::string() : s->second;
        const std::string theirs = c == clientData.end() ? std::string() : c->second;
        if (ours == theirs)
            continue;
        differ += (differ.empty() ? "" : ", ") + name;
        if (theirs.empty())
            differ += " (missing on your side)";
        else if (ours.empty())
            differ += " (missing on the server)";
    }
    if (differ.empty())
        return std::string();
    return "Your game data differs from this server's in " + differ + ", though both are " +
           Named(serverBuild) + ": reinstall " + Named(serverBuild) + " from " +
           Version::RELEASES_URL + " or copy the server's data folder";
}

}  // namespace Compat
