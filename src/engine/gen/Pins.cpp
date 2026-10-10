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

// The index in `list` that `ref` names: a name (the first object called that) or a
// position. -1 if nothing.
int Resolve(const json& list, const json& ref)
{
    if (ref.is_number_integer())
    {
        const long long i = ref.get<long long>();
        return i >= 0 && i < (long long)list.size() ? (int)i : -1;
    }
    if (!ref.is_string() || ref.get<std::string>().empty())
        return -1;
    for (size_t i = 0; i < list.size(); i++)
        if (list[i].is_object() && list[i].value("name", std::string()) == ref.get<std::string>())
            return (int)i;
    return -1;
}

// One list of a merge. Every "replaces" is resolved against the list as it stood before
// this pin, then the list is rebuilt: kept and replaced objects in their places, removed
// ones gone, added ones after. `origins` (may be null) is carried through the same way.
void MergeList(json& list, const json& entries, const std::string& where, const std::string& key,
               std::vector<std::string>& problems, std::vector<int>* origins)
{
    if (!list.is_array())
        list = json::array();
    std::vector<int> from;
    if (origins != nullptr && origins->size() == list.size())
        from = *origins;
    else
        for (size_t i = 0; i < list.size(); i++)
            from.push_back((int)i);

    std::vector<int>  fate(list.size(), 0);  // 0 kept, 1 replaced, 2 removed
    std::vector<json> replacement(list.size());
    std::vector<json> added;
    for (json o : entries)
    {
        if (!o.is_object())
        {
            problems.push_back(where + ": an entry of \"" + key + "\" is not an object");
            continue;
        }
        if (!o.contains("replaces"))
        {
            added.push_back(o);
            continue;
        }
        const json ref = o["replaces"];
        const bool remove = o.value("remove", false);
        o.erase("replaces");
        o.erase("remove");
        const int at = Resolve(list, ref);
        if (at < 0)
        {
            problems.push_back(where + ": nothing called " + ref.dump() + " in \"" + key +
                               "\" to replace");
            continue;
        }
        if (fate[(size_t)at] != 0)
        {
            problems.push_back(where + ": " + ref.dump() + " in \"" + key + "\" is replaced twice");
            continue;
        }
        fate[(size_t)at] = remove ? 2 : 1;
        replacement[(size_t)at] = o;
    }

    json             out = json::array();
    std::vector<int> outFrom;
    for (size_t i = 0; i < list.size(); i++)
    {
        if (fate[i] == 2)
            continue;
        out.push_back(fate[i] == 1 ? replacement[i] : list[i]);
        outFrom.push_back(from[i]);
    }
    for (const json& o : added)
    {
        out.push_back(o);
        outFrom.push_back(-1);
    }
    list = out;
    if (origins != nullptr)
        *origins = outFrom;
}

void Merge(json& sys, const json& doc, const std::string& where, std::vector<std::string>& problems,
           Origins* origins)
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
        MergeList(sys[key], it.value(), where, key, problems,
                  origins != nullptr ? &(*origins)[key] : nullptr);
    }
}

// Applies one pin's document to one system. False if the pin itself is malformed.
bool ApplyOne(json& sys, const json& pin, const std::string& where,
              std::vector<std::string>& problems, Origins* origins)
{
    const std::string mode = pin.value("mode", std::string("merge"));
    if (mode == "merge")
        Merge(sys, pin["document"], where, problems, origins);
    else if (mode == "replace")
    {
        json doc = pin["document"];
        if (!doc.contains("gates") && sys.contains("gates"))
            doc["gates"] = sys["gates"];  // the topology is the region's
        if (origins != nullptr)
        {
            // Nothing in the document came from what was there -- except the kept gates.
            const std::vector<int> gates = (*origins)["gates"];
            *origins = IdentityOrigins(doc);
            for (auto& kv : *origins)
                for (int& o : kv.second)
                    o = -1;
            if (!pin["document"].contains("gates"))
                (*origins)["gates"] = gates;
        }
        sys = doc;
    }
    else
    {
        problems.push_back(where + ": mode is \"merge\" or \"replace\", not \"" + mode + "\"");
        return false;
    }
    return true;
}

// The checks every pin goes through before it may apply; each failure is a problem.
bool WellFormed(const json& pin, const std::string& where, std::vector<std::string>& problems)
{
    if (!pin.is_object() || !pin.contains("system") || !pin["system"].is_string() ||
        !pin.contains("document") || !pin["document"].is_object())
    {
        problems.push_back(where + ": needs \"system\" and a \"document\" object");
        return false;
    }
    bool ok = true;
    for (auto it = pin.begin(); it != pin.end(); ++it)
        if (!PIN_FIELDS.count(it.key()))
        {
            // A misspelt field would otherwise read as an absent one (#191).
            problems.push_back(where + ": unknown field \"" + it.key() + "\"");
            ok = false;
        }
    if (ok && pin.contains("seed") &&
        !(pin["seed"].is_number_integer() && pin["seed"].get<long long>() >= 0))
    {
        problems.push_back(where + ": \"seed\" must be a whole number");
        ok = false;
    }
    return ok;
}

std::string Where(const json& pin, int n)
{
    return "pin " + std::to_string(n) +
           (pin.is_object() && pin.contains("system") && pin["system"].is_string()
                ? " (" + pin["system"].get<std::string>() + ")"
                : std::string());
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
        const std::string where = Where(pin, ++n);
        if (!WellFormed(pin, where, problems))
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
        ApplyOne(sys->second, pin, where, problems, nullptr);
    }
}

// ---- The editor's side (#237) ---------------------------------------------------------

Origins IdentityOrigins(const json& doc)
{
    Origins out;
    for (const std::string& key : OBJECT_LISTS)
        if (doc.contains(key) && doc[key].is_array())
            for (size_t i = 0; i < doc[key].size(); i++)
                out[key].push_back((int)i);
    return out;
}

bool OwnedPin(const json& pin, const std::string& system, uint64_t seed)
{
    return pin.is_object() && pin.value("system", std::string()) == system &&
           pin.contains("seed") && pin["seed"].is_number_integer() &&
           pin["seed"].get<uint64_t>() == seed;
}

bool OpenForEditing(const Region& generated, const json& pins, uint64_t seed,
                    const std::string& system, EditedSystem& out)
{
    if (!generated.documents.count(system))
        return false;
    out = EditedSystem();

    // Everything the editor does not own, in file order, as the server applies it.
    Region others = generated;
    json   owned = json::array();
    if (pins.contains("pins") && pins["pins"].is_array())
    {
        json rest = { { "pins", json::array() } };
        for (const json& pin : pins["pins"])
            (OwnedPin(pin, system, seed) ? owned : rest["pins"]).push_back(pin);
        ApplyPins(others, rest, seed, out.problems);
    }
    out.base = others.documents.at(system);
    out.edited = out.base;
    out.origins = IdentityOrigins(out.base);

    // Then its own, with where every object came from carried through them.
    int n = 0;
    for (const json& pin : owned)
    {
        const std::string where = "owned " + Where(pin, ++n);
        if (WellFormed(pin, where, out.problems))
            ApplyOne(out.edited, pin, where, out.problems, &out.origins);
    }
    return true;
}

json PinFor(const std::string& system, uint64_t seed, const json& base, const json& edited,
            const Origins& origins)
{
    json doc = json::object();
    bool expressible = true;

    for (auto it = edited.begin(); it != edited.end(); ++it)
        if (!OBJECT_LISTS.count(it.key()) &&
            (!base.contains(it.key()) || base[it.key()] != it.value()))
            doc[it.key()] = it.value();
    for (auto it = base.begin(); it != base.end(); ++it)
        if (!OBJECT_LISTS.count(it.key()) && !edited.contains(it.key()))
            expressible = false;  // a merge cannot take a key away

    for (const std::string& key : OBJECT_LISTS)
    {
        const json       empty = json::array();
        const json&      was = base.contains(key) && base[key].is_array() ? base[key] : empty;
        const json&      now = edited.contains(key) && edited[key].is_array() ? edited[key] : empty;
        std::vector<int> from;
        const auto       o = origins.find(key);
        if (o != origins.end())
            from = o->second;
        if (from.size() != now.size())
        {
            expressible = false;  // no record of where these came from
            continue;
        }

        // A merge keeps what survives in its old order and puts what is new after it: an
        // edit that reordered the list cannot be said as one.
        int  last = -1;
        bool addedYet = false;
        for (int f : from)
        {
            if (f < -1 || f >= (int)was.size() || (f >= 0 && (f <= last || addedYet)))
                expressible = false;
            if (f >= 0)
                last = f;
            else
                addedYet = true;
        }
        if (!expressible)
            continue;

        // An object is named by its name when that is unique, by its place otherwise.
        auto ref = [&](int i) -> json
        {
            const std::string name =
                was[(size_t)i].is_object() ? was[(size_t)i].value("name", std::string()) : "";
            if (!name.empty() && Resolve(was, name) == i)
                return name;
            return i;
        };
        std::vector<bool> kept(was.size(), false);
        json              entries = json::array();
        for (size_t j = 0; j < now.size(); j++)
            if (from[j] >= 0)
            {
                kept[(size_t)from[j]] = true;
                if (now[j] != was[(size_t)from[j]])
                {
                    json e = now[j];
                    e["replaces"] = ref(from[j]);
                    entries.push_back(e);
                }
            }
        for (size_t i = 0; i < was.size(); i++)
            if (!kept[i])
                entries.push_back({ { "replaces", ref((int)i) }, { "remove", true } });
        for (size_t j = 0; j < now.size(); j++)
            if (from[j] < 0)
                entries.push_back(now[j]);
        if (!entries.empty())
            doc[key] = entries;
    }

    json pin = { { "system", system },
                 { "seed", seed },
                 { "note", "written by the world editor (#237)" } };
    if (!expressible)
    {
        pin["mode"] = "replace";
        pin["document"] = edited;
        return pin;
    }
    if (doc.empty())
        return json();
    pin["mode"] = "merge";
    pin["document"] = doc;
    return pin;
}

void StorePin(json& pinsFile, const std::string& system, uint64_t seed, const json& pin)
{
    if (!pinsFile.is_object())
        pinsFile = json::object();
    json kept = json::array();
    if (pinsFile.contains("pins") && pinsFile["pins"].is_array())
        for (const json& p : pinsFile["pins"])
            if (!OwnedPin(p, system, seed))
                kept.push_back(p);
    // Last, because the editor showed it on top of every other pin.
    if (!pin.is_null())
        kept.push_back(pin);
    pinsFile["pins"] = kept;
}
}  // namespace Gen
