// Writing a pack (#342 S8): a region of the world taken out as an admin pack, or one begun
// from nothing, in exactly the files LoadPack reads. The editor's side of Pack.h; nothing here
// runs in a server.

#include "gen/Pack.h"

#include "core/Faction.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>

namespace Gen
{
namespace
{
using nlohmann::json;

// "r14-2-3" -> "s2-3": the slot is the weaving's to give, the rest says where it lay.
std::string PackSystemId(const std::string& id)
{
    if (SlotOf(id) < 0)
        return id;
    const size_t dash = id.find('-');
    return "s" + id.substr(dash + 1);
}

// The natives as factions.json spells a faction, under the pack's own id.
json NativesFaction(const json& natives, const std::string& id)
{
    json f = { { "id", id },
               { "name", natives.value("name", id) },
               { "kind", "Native" },
               { "lawful", natives.value("lawful", true) } };
    // Their culture, when the registry knows it: a pack's faction may name only one it has.
    const std::string culture = natives.value("culture", std::string());
    if (Factions::Culture(culture) != nullptr)
        f["culture"] = culture;
    const json c = natives.value("color", json());
    if (c.is_array() && c.size() >= 3)
    {
        json rgb = json::array();
        for (size_t i = 0; i < 3; i++)
            rgb.push_back(c[i].is_number() ? std::max(0, std::min(255, c[i].get<int>())) : 200);
        f["color"] = rgb;
    }
    const json t = natives.value("temperament", json());
    if (t.is_object())
    {
        // Only what a faction's temperament may say: anything else would be refused.
        json m = json::object();
        for (const char* k : { "appetite", "capacity", "growth", "curiosity" })
            if (t.contains(k) && t[k].is_number())
                m[k] = t[k];
        json values = json::object();
        if (t.contains("values") && t["values"].is_object())
            for (const char* k : { "traffic", "ore", "salvage", "unclaimed" })
                if (t["values"].contains(k) && t["values"][k].is_number())
                    values[k] = t["values"][k];
        m["values"] = values;
        f["temperament"] = m;
    }
    return f;
}
}  // namespace

PackFiles RegionAsPack(const Region& region, const std::string& id, const std::string& name,
                       std::vector<std::string>* notes)
{
    auto note = [notes](const std::string& line)
    {
        if (notes != nullptr)
            notes->push_back(line);
    };
    PackFiles out;

    // The way in first: a region pack is entered through its first system.
    std::vector<const json*> order;
    for (const json& s : region.systems)
        if (s.value("id", std::string()) == region.entryId)
            order.push_back(&s);
    for (const json& s : region.systems)
        if (s.value("id", std::string()) != region.entryId)
            order.push_back(&s);
    std::map<std::string, std::string> rename;
    for (const json* s : order)
        rename[s->value("id", std::string())] = PackSystemId(s->value("id", std::string()));

    // The natives' faction, renamed: the generated id is a hash of a slot of one world.
    std::map<std::string, std::string> faction;
    json                               factions = json::array(), relations = json::array();
    if (region.natives.is_object() && region.natives.contains("id") &&
        region.natives["id"].is_string())
    {
        const std::string nid = id + "-natives";
        faction[region.natives["id"].get<std::string>()] = nid;
        factions.push_back(NativesFaction(region.natives, nid));
        const json        rel = region.natives.value("relations", json::object());
        const std::string majors =
            rel.is_object() ? rel.value("majors", std::string("Neutral")) : std::string("Neutral");
        const std::string pirates =
            rel.is_object() ? rel.value("pirates", std::string("Hostile")) : std::string("Hostile");
        for (const std::string& m : Factions::MajorIds())
            relations.push_back(json::array({ nid, m, majors }));
        relations.push_back(json::array({ nid, Factions::Id(Factions::Pirates()), pirates }));
    }
    for (const json& f : region.factions)  // a region a pack wove in: its own, unchanged
        if (f.is_object())
            factions.push_back(f);
    for (const json& r : region.relations)
        relations.push_back(r);
    auto factionOf = [&faction](const std::string& f)
    {
        const auto it = faction.find(f);
        return it != faction.end() ? it->second : f;
    };

    // The map, round its middle and drawn in to the reach a pack may have.
    double x1 = 0.0, y1 = 0.0, x2 = 0.0, y2 = 0.0;
    bool   first = true;
    for (const json* s : order)
    {
        const json& m = s->value("map", json::array());
        if (!m.is_array() || m.size() < 2 || !m[0].is_number() || !m[1].is_number())
            continue;
        const double x = m[0].get<double>(), y = m[1].get<double>();
        x1 = first ? x : std::min(x1, x);
        y1 = first ? y : std::min(y1, y);
        x2 = first ? x : std::max(x2, x);
        y2 = first ? y : std::max(y2, y);
        first = false;
    }
    const double cx = (x1 + x2) / 2.0, cy = (y1 + y2) / 2.0;
    const double half = std::max(x2 - x1, y2 - y1) / 2.0;
    const double reach = PACK_MAP_REACH - 20.0;
    const double scale = half > reach ? reach / half : 1.0;
    if (scale < 1.0)
        note("the map drawn in to " + std::to_string((int)(scale * 100.0)) +
             "% to stay within a pack's reach");

    json systems = json::array();
    for (const json* s : order)
    {
        const std::string old = s->value("id", std::string());
        const std::string pid = rename[old];
        json              e = { { "id", pid }, { "file", pid + ".json" } };
        const json&       m = s->value("map", json::array());
        if (m.is_array() && m.size() >= 2 && m[0].is_number() && m[1].is_number())
            e["map"] = json::array({ (int)std::lround((m[0].get<double>() - cx) * scale),
                                     (int)std::lround((m[1].get<double>() - cy) * scale) });
        // A designation is the slot's ("R6 W-2.1"), not the system's: left out, the system is
        // designated afresh wherever it is woven. A name somebody gave it goes with it.
        const std::string given = s->value("name", std::string());
        const size_t      lastDash = old.rfind('-');
        const int         k = SlotOf(old) >= 0 ? std::atoi(old.c_str() + lastDash + 1) : 0;
        if (!given.empty() &&
            (SlotOf(old) < 0 || given != SystemDesignation(SlotOf(old), DepthOf(old), k)))
            e["name"] = given;
        e["security"] = std::max(0.0, std::min(1.0, s->value("security", 0.5)));
        const std::string owner = s->value("owner", std::string());
        if (!owner.empty())
            e["owner"] = factionOf(owner);
        systems.push_back(e);

        // The document, its gates among the pack's own systems and its stations' flags renamed.
        const auto doc = region.documents.find(old);
        json       d = doc != region.documents.end() ? doc->second : json::object();
        if (d.contains("gates") && d["gates"].is_array())
        {
            json kept = json::array();
            for (json g : d["gates"])
            {
                const std::string to = g.value("destination", std::string());
                if (rename.count(to) == 0)
                {
                    note(pid + ": the gate to " + to + " leads out of the region -- left out");
                    continue;
                }
                g["destination"] = rename[to];
                g.erase("expiresAt");  // a mouth's, dressed by the server, never written
                kept.push_back(g);
            }
            d["gates"] = kept;
        }
        if (d.contains("stations") && d["stations"].is_array())
            for (json& st : d["stations"])
                if (st.is_object() && st.contains("faction") && st["faction"].is_string())
                    st["faction"] = factionOf(st["faction"].get<std::string>());
        out.documents[pid + ".json"] = d;
    }

    json links = json::array();
    for (const json& l : region.links)
    {
        if (!l.is_array() || l.size() != 2 || !l[0].is_string() || !l[1].is_string())
            continue;
        const std::string a = l[0].get<std::string>(), b = l[1].get<std::string>();
        if (rename.count(a) != 0 && rename.count(b) != 0)
            links.push_back(json::array({ rename[a], rename[b] }));
    }

    json                  starts = json::array();
    std::set<std::string> started;
    for (const std::string& s : region.starts)
        if (rename.count(s) != 0 && started.insert(rename[s]).second)
            starts.push_back(rename[s]);
    if (rename.count(region.heartId) != 0 && started.insert(rename[region.heartId]).second)
        starts.push_back(rename[region.heartId]);

    out.pack = {
        { "schema", PACK_SCHEMA },
        { "id", id },
        { "name", name.empty() ? id : name },
        { "kind", "region" },
        { "description", "Region " + std::to_string(region.slot) +
                             " of a generated world, exported by the editor (#342 S8)." },
        { "systems", systems },
        { "links", links },
        { "factions", factions },
        { "relations", relations },
        { "start", starts },
        { "weave",
          { { "place", "random" },
            { "links", { { "count", json::array({ 1, 2 }) }, { "kind", "static" } } } } }
    };
    return out;
}

PackFiles NewRegionPack(const std::string& id, const std::string& name)
{
    PackFiles         out;
    const std::string label = name.empty() ? id : name;
    out.documents["core.json"] = {
        { "star", { { "type", "Yellow" }, { "size", 150000 } } },
        { "planets", json::array({ { { "type", "Rocky" },
                                     { "orbitRadius", 320000 },
                                     { "orbitSpeed", 30 },
                                     { "angle", 0.6 },
                                     { "size", 14000 },
                                     { "deposit", "Iron" } },
                                   { { "type", "Gas" },
                                     { "orbitRadius", 640000 },
                                     { "orbitSpeed", 40 },
                                     { "angle", 3.4 },
                                     { "size", 32000 },
                                     { "deposit", "Ice" } } }) },
        { "stations",
          json::array(
              { { { "name", label + " Station" },
                  { "orbits",
                    { { "planet", 0 }, { "radius", 24000 }, { "speed", 30 }, { "phase", 0.0 } } },
                  { "size", 700 },
                  { "faction", Factions::Id(Factions::Unaffiliated()) },
                  { "role", "TradeHub" } } }) },
        { "gates", json::array() }
    };
    out.pack = {
        { "schema", PACK_SCHEMA },
        { "id", id },
        { "name", label },
        { "kind", "region" },
        { "description", "Begun in the editor (#342 S8)." },
        { "systems", json::array({ { { "id", "core" },
                                     { "file", "core.json" },
                                     { "map", json::array({ 0, 0 }) },
                                     { "name", label + " Core" },
                                     { "security", 0.8 } } }) },
        { "links", json::array() },
        { "factions", json::array() },
        { "relations", json::array() },
        { "start", json::array({ "core" }) },
        { "weave",
          { { "place", "random" },
            { "links", { { "count", json::array({ 1, 2 }) }, { "kind", "static" } } } } }
    };
    return out;
}

bool WritePack(const std::string& dirIn, const PackFiles& files, bool overwrite, std::string& error)
{
    namespace fs = std::filesystem;
    const fs::path  dir(dirIn);
    std::error_code ec;
    if (!overwrite && fs::exists(dir / "pack.json", ec))
    {
        error = dir.generic_string() + " already holds a pack";
        return false;
    }
    fs::create_directories(dir, ec);
    if (ec)
    {
        error = "cannot make " + dir.generic_string() + ": " + ec.message();
        return false;
    }
    auto write = [&error](const fs::path& path, const json& j)
    {
        std::ofstream out(path);
        if (!out.is_open())
        {
            error = "cannot write " + path.generic_string();
            return false;
        }
        out << j.dump(4) << "\n";
        return true;
    };
    if (!write(dir / "pack.json", files.pack))
        return false;
    for (const auto& kv : files.documents)
        if (!write(dir / kv.first, kv.second))
            return false;
    return true;
}

}  // namespace Gen
