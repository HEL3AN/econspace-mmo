#include "gen/Pins.h"

#include <set>

namespace Gen
{
namespace
{
using json = nlohmann::json;

const std::set<std::string> OBJECT_LISTS = { "planets", "stations",  "asteroidFields",
                                             "nebulae", "derelicts", "gates" };
const std::set<std::string> PIN_FIELDS = { "system", "seed", "mode", "document", "note" };

void Merge(json& sys, const json& doc, const std::string& where, std::vector<std::string>& problems)
{
    for (auto it = doc.begin(); it != doc.end(); ++it)
    {
        const std::string& key = it.key();
        if (!OBJECT_LISTS.count(key))
        {
            sys[key] = it.value();  // star, stars, character: the pin's word stands
            continue;
        }
        if (!it.value().is_array())
        {
            problems.push_back(where + ": \"" + key + "\" must be a list");
            continue;
        }
        json& list = sys[key];
        if (!list.is_array())
            list = json::array();
        for (json o : it.value())
        {
            if (!o.is_object())
            {
                problems.push_back(where + ": an entry of \"" + key + "\" is not an object");
                continue;
            }
            if (!o.contains("replaces"))
            {
                list.push_back(o);
                continue;
            }
            const std::string target =
                o["replaces"].is_string() ? o["replaces"].get<std::string>() : std::string();
            const bool remove = o.value("remove", false);
            o.erase("replaces");
            o.erase("remove");
            bool found = false;
            for (size_t i = 0; i < list.size(); i++)
                if (list[i].value("name", std::string()) == target)
                {
                    if (remove)
                        list.erase(list.begin() + (std::ptrdiff_t)i);
                    else
                        list[i] = o;
                    found = true;
                    break;
                }
            if (!found)
                problems.push_back(where + ": nothing called \"" + target + "\" in \"" + key +
                                   "\" to replace");
        }
    }
}
}  // namespace

void ApplyPins(Region& region, const json& pins, uint64_t seed, std::vector<std::string>& problems)
{
    if (!pins.contains("pins"))
        return;
    if (!pins["pins"].is_array())
    {
        problems.push_back("pins.json: \"pins\" must be a list");
        return;
    }
    int n = 0;
    for (const json& pin : pins["pins"])
    {
        const std::string where =
            "pin " + std::to_string(++n) +
            (pin.is_object() && pin.contains("system") && pin["system"].is_string()
                 ? " (" + pin["system"].get<std::string>() + ")"
                 : std::string());
        if (!pin.is_object() || !pin.contains("system") || !pin["system"].is_string() ||
            !pin.contains("document") || !pin["document"].is_object())
        {
            problems.push_back(where + ": needs \"system\" and a \"document\" object");
            continue;
        }
        bool unknown = false;
        for (auto it = pin.begin(); it != pin.end(); ++it)
            if (!PIN_FIELDS.count(it.key()))
            {
                // A misspelt field would otherwise read as an absent one (#191).
                problems.push_back(where + ": unknown field \"" + it.key() + "\"");
                unknown = true;
            }
        if (unknown)
            continue;
        if (pin.contains("seed") && pin["seed"].get<uint64_t>() != seed)
            continue;  // for another region

        const std::string id = pin["system"];
        auto              sys = region.documents.find(id);
        if (sys == region.documents.end())
        {
            problems.push_back(where + ": this region has no system \"" + id + "\"");
            continue;
        }
        const std::string mode = pin.value("mode", std::string("merge"));
        if (mode == "merge")
            Merge(sys->second, pin["document"], where, problems);
        else if (mode == "replace")
        {
            json doc = pin["document"];
            if (!doc.contains("gates") && sys->second.contains("gates"))
                doc["gates"] = sys->second["gates"];  // the topology is the region's
            sys->second = doc;
        }
        else
            problems.push_back(where + ": mode is \"merge\" or \"replace\", not \"" + mode + "\"");
    }
}
}  // namespace Gen
