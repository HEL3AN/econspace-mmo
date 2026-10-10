#include "gen/Pins.h"

#include <map>
#include <set>

namespace Gen
{
namespace
{
using json = nlohmann::json;

const std::set<std::string> OBJECT_LISTS = { "planets", "stations",  "asteroidFields",
                                             "nebulae", "derelicts", "gates" };
const std::set<std::string> PIN_FIELDS = { "system", "seed", "mode", "document", "note" };

// The record of one list, if `origins` has one -- looked up, never made: an empty list has
// no entry, and making one would make two equal records compare unequal.
std::vector<int>* OriginsOf(Origins* origins, const std::string& key)
{
    if (origins == nullptr)
        return nullptr;
    const auto o = origins->find(key);
    return o != origins->end() ? &o->second : nullptr;
}

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
// `prior` gets, for each object of the new list, its index before the pin (-1 if added);
// `untouched`, whether the pin left it as it was -- the only objects whose references the
// merge itself still has to keep right.
void MergeList(json& list, const json& entries, const std::string& where, const std::string& key,
               std::vector<std::string>& problems, std::vector<int>* origins,
               std::vector<int>& prior, std::vector<bool>& untouched)
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
    prior.clear();
    untouched.clear();
    for (size_t i = 0; i < list.size(); i++)
    {
        if (fate[i] == 2)
            continue;
        out.push_back(fate[i] == 1 ? replacement[i] : list[i]);
        outFrom.push_back(from[i]);
        prior.push_back((int)i);
        untouched.push_back(fate[i] == 0);
    }
    for (const json& o : added)
    {
        out.push_back(o);
        outFrom.push_back(-1);
        prior.push_back(-1);
        untouched.push_back(false);
    }
    list = out;
    if (origins != nullptr)
        *origins = outFrom;
}

void Merge(json& sys, const json& doc, const std::string& where, std::vector<std::string>& problems,
           Origins* origins)
{
    const size_t planetsBefore =
        sys.contains("planets") && sys["planets"].is_array() ? sys["planets"].size() : 0;
    std::map<std::string, std::vector<bool>> untouched;  // per list the pin merged
    std::vector<int>                         planetPrior;
    bool                                     planetsMerged = false;
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
        std::vector<int> prior;
        MergeList(sys[key], it.value(), where, key, problems,
                  origins != nullptr ? &(*origins)[key] : nullptr, prior, untouched[key]);
        if (key == "planets")
        {
            planetPrior = prior;
            planetsMerged = true;
        }
    }
    if (!planetsMerged)
        return;

    // A satellite belongs to its planet (#210) and finds it by its place in the list, so
    // what the pin did to the planets is done to what it left alone: re-pointed at its
    // planet's new place, or gone with it. An object the pin wrote is as the pin wrote it.
    std::vector<int> map(planetsBefore, -1);
    for (size_t j = 0; j < planetPrior.size(); j++)
        if (planetPrior[j] >= 0 && planetPrior[j] < (int)planetsBefore)
            map[(size_t)planetPrior[j]] = (int)j;
    for (const std::string& key : OBJECT_LISTS)
    {
        if (key == "planets" || !sys.contains(key) || !sys[key].is_array())
            continue;
        const auto        u = untouched.find(key);
        std::vector<int>* from = OriginsOf(origins, key);
        json              kept = json::array();
        std::vector<int>  keptFrom;
        for (size_t i = 0; i < sys[key].size(); i++)
        {
            const bool alone = u == untouched.end() || i >= u->second.size() || u->second[i];
            const json o = alone ? Repointed(sys[key][i], map) : sys[key][i];
            if (o.is_null())
                continue;
            kept.push_back(o);
            if (from != nullptr && i < from->size())
                keptFrom.push_back((*from)[i]);
        }
        if (from != nullptr && from->size() == sys[key].size())
            *from = keptFrom;
        sys[key] = kept;
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

// Applies one pin, then holds the result to CheckPinned: a pin that would break the
// system is not applied at all, rather than applied and broken.
void ApplyChecked(json& sys, const json& pin, const std::string& where,
                  const std::set<std::string>& systems, std::vector<std::string>& problems,
                  Origins* origins)
{
    json    trial = sys;
    Origins trialOrigins = origins != nullptr ? *origins : Origins();
    if (!ApplyOne(trial, pin, where, problems, origins != nullptr ? &trialOrigins : nullptr))
        return;
    const size_t before = problems.size();
    CheckPinned(sys, trial, systems, where, problems);
    if (problems.size() != before)
        return;
    sys = trial;
    if (origins != nullptr)
        *origins = trialOrigins;
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
    const std::set<std::string> systems = Destinations(region);
    int                         n = 0;
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
        ApplyChecked(sys->second, pin, where, systems, problems, nullptr);
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
    const std::set<std::string> systems = Destinations(generated);

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
            ApplyChecked(out.edited, pin, where, systems, out.problems, &out.origins);
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

    // What a merge does by itself to an object it is not told about: re-points a
    // satellite at its planet's new place, or removes it with its planet.
    const size_t basePlanets =
        base.contains("planets") && base["planets"].is_array() ? base["planets"].size() : 0;
    const std::vector<int> planetMap = PlanetMap(basePlanets, origins);

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
                const json alone = Repointed(was[(size_t)from[j]], planetMap);
                if (alone.is_null() || now[j] != alone)
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

// ---- What a pinned system must still be (#237) ----------------------------------------

std::vector<int> PlanetMap(size_t basePlanets, const Origins& origins)
{
    std::vector<int> map(basePlanets, -1);
    const auto       o = origins.find("planets");
    if (o == origins.end())
    {
        for (size_t i = 0; i < basePlanets; i++)
            map[i] = (int)i;
        return map;
    }
    for (size_t j = 0; j < o->second.size(); j++)
        if (o->second[j] >= 0 && o->second[j] < (int)basePlanets)
            map[(size_t)o->second[j]] = (int)j;
    return map;
}

json Repointed(const json& obj, const std::vector<int>& planetMap)
{
    if (!obj.is_object() || !obj.contains("orbits") || !obj["orbits"].is_object() ||
        !obj["orbits"].contains("planet") || !obj["orbits"]["planet"].is_number_integer())
        return obj;
    const long long p = obj["orbits"]["planet"].get<long long>();
    if (p < 0 || p >= (long long)planetMap.size())
        return obj;  // already wrong: CheckPinned says so
    if (planetMap[(size_t)p] < 0)
        return json();
    json out = obj;
    out["orbits"]["planet"] = planetMap[(size_t)p];
    return out;
}

int RemovePlanet(json& sys, int index, Origins* origins)
{
    if (!sys.contains("planets") || !sys["planets"].is_array() || index < 0 ||
        index >= (int)sys["planets"].size())
        return 0;
    const size_t n = sys["planets"].size();
    sys["planets"].erase((size_t)index);
    if (origins != nullptr)
    {
        std::vector<int>& from = (*origins)["planets"];
        if (index < (int)from.size())
            from.erase(from.begin() + index);
    }
    std::vector<int> map(n, -1);
    for (size_t i = 0; i < n; i++)
        if ((int)i != index)
            map[i] = (int)i < index ? (int)i : (int)i - 1;

    int gone = 0;
    for (const std::string& key : OBJECT_LISTS)
    {
        if (key == "planets" || !sys.contains(key) || !sys[key].is_array())
            continue;
        std::vector<int>* from = OriginsOf(origins, key);
        for (size_t i = sys[key].size(); i-- > 0;)
        {
            const json o = Repointed(sys[key][i], map);
            if (!o.is_null())
            {
                sys[key][i] = o;
                continue;
            }
            sys[key].erase(i);
            if (from != nullptr && i < from->size())
                from->erase(from->begin() + (long)i);
            gone++;
        }
    }
    return gone;
}

std::set<std::string> Destinations(const Region& region)
{
    std::set<std::string> out;
    for (const json& s : region.systems)
        if (s.is_object() && s.contains("id") && s["id"].is_string())
            out.insert(s["id"].get<std::string>());
    for (const json& l : region.links)
        if (l.is_array())
            for (const json& end : l)
                if (end.is_string())
                    out.insert(end.get<std::string>());
    for (const auto& kv : region.documents)
        out.insert(kv.first);
    return out;
}

void CheckPinned(const json& before, const json& after, const std::set<std::string>& systems,
                 const std::string& where, std::vector<std::string>& problems)
{
    // Gates are the region's topology: a pin may move one, rename it, change its look,
    // never where it leads -- the far side would keep a gate back to a system that no
    // longer comes to it, or a gate would lead nowhere.
    auto leads = [](const json& sys)
    {
        std::multiset<std::string> out;
        if (sys.contains("gates") && sys["gates"].is_array())
            for (const json& g : sys["gates"])
                out.insert(g.is_object() ? g.value("destination", std::string()) : std::string());
        return out;
    };
    const std::multiset<std::string> was = leads(before), now = leads(after);
    for (const std::string& d : now)
        if (!systems.count(d))
            problems.push_back(where + ": a gate leads to \"" + d +
                               "\", and this region has no such system");
    if (was != now)
    {
        std::string a, b;
        for (const std::string& d : was)
            a += (a.empty() ? "" : ", ") + d;
        for (const std::string& d : now)
            b += (b.empty() ? "" : ", ") + d;
        problems.push_back(where + ": the gates would lead to [" + b + "] instead of [" + a +
                           "] -- a pin may move a gate, never change where it leads");
    }

    // Every satellite's planet is there (#210): it is found by its place in the list.
    const long long planets = after.contains("planets") && after["planets"].is_array()
                                  ? (long long)after["planets"].size()
                                  : 0;
    for (const std::string& key : OBJECT_LISTS)
    {
        if (key == "planets" || !after.contains(key) || !after[key].is_array())
            continue;
        for (const json& o : after[key])
        {
            if (!o.is_object() || !o.contains("orbits") || !o["orbits"].is_object())
                continue;
            const json p = o["orbits"].value("planet", json());
            if (p.is_number_integer() && p.get<long long>() >= 0 && p.get<long long>() < planets)
                continue;
            problems.push_back(where + ": \"" + o.value("name", std::string("an object")) +
                               "\" in \"" + key + "\" orbits planet " + p.dump() +
                               ", and the system has " + std::to_string(planets));
        }
    }
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
