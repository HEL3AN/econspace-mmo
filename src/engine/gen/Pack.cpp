#include "gen/Pack.h"

#include "core/Archetype.h"
#include "core/Faction.h"
#include "core/World.h"
#include "core/WorldLoader.h"
#include "entities/Entity.h"
#include "entities/Planet.h"
#include "entities/Station.h"
#include "gen/Pins.h"
#include "gen/Rng.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>

namespace Gen
{
namespace
{
using nlohmann::json;

// Purposes, apart from every key a region or the skeleton uses.
constexpr uint64_t PACK_PLACE = 0x9AC0001ull;
constexpr uint64_t PACK_LINKS = 0x9AC0002ull;
constexpr uint64_t PACK_GATE = 0x9AC0003ull;
constexpr uint64_t PACK_SYSTEM = 0x9AC0004ull;

// FNV-1a: the same 64 bits for the same bytes everywhere, which std::hash does not promise.
uint64_t Fnv(const std::string& text, uint64_t h = 0xCBF29CE484222325ull)
{
    for (unsigned char c : text)
    {
        h ^= c;
        h *= 0x100000001B3ull;
    }
    return h;
}

// A key for a pack: its id, never its place in a list.
uint64_t IdKey(const Pack& pack)
{
    return Fnv(pack.id);
}

std::string Quoted(const std::string& s)
{
    return "\"" + s + "\"";
}

// Every key of `j` is one of `known`; the first that is not is said.
bool Known(const json& j, const std::vector<const char*>& known, const std::string& where,
           std::vector<std::string>& problems)
{
    bool ok = true;
    for (auto it = j.begin(); it != j.end(); ++it)
    {
        bool found = false;
        for (const char* k : known)
            found = found || it.key() == k;
        if (!found)
        {
            problems.push_back(where + ": unknown field " + Quoted(it.key()));
            ok = false;
        }
    }
    return ok;
}

bool IsPoint(const json& j)
{
    return j.is_array() && j.size() == 2 && j[0].is_number() && j[1].is_number();
}

bool PackIdOk(const std::string& id)
{
    if (id.empty() || id.size() > 40)
        return false;
    for (char c : id)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_'))
            return false;
    return true;
}

// The kinds of star and of ore the loader knows; anything else it would quietly make the
// first of them.
void StarKind(const std::string& type, const std::string& where, std::vector<std::string>& problems)
{
    if (type != "Yellow" && type != "Red" && type != "Blue")
        problems.push_back(where + ": a star's \"type\" is \"Yellow\", \"Red\" or \"Blue\"");
}

bool IsResource(const std::string& r)
{
    return r == "Iron" || r == "Ice" || r == "Crystal";
}

// What each list of a system document may hold, and what each object of it must say: the
// fields WorldLoader::BuildSystem reads (#191 -- a misspelling read with a default is
// indistinguishable from an absence).
struct ListRule
{
    const char*              list;
    std::vector<const char*> known;
    std::vector<const char*> required;
};

const ListRule LISTS[] = {
    { "stars", { "type", "size", "pos", "archetype" }, {} },
    { "planets",
      { "type", "color", "orbitRadius", "orbitSpeed", "angle", "size", "deposit", "archetype" },
      { "orbitRadius", "size" } },
    { "stations",
      { "name", "pos", "size", "faction", "role", "archetype", "orbits" },
      { "name", "size" } },
    { "asteroidFields",
      { "name", "pos", "size", "resource", "ore", "archetype", "orbits" },
      { "name", "size", "resource", "ore" } },
    { "nebulae", { "name", "pos", "radius", "archetype", "orbits" }, { "radius" } },
    { "derelicts",
      { "name", "pos", "size", "reward", "expiresAt", "archetype", "orbits" },
      { "name" } },
    { "gates",
      { "name", "pos", "size", "destination", "archetype" },
      { "name", "pos", "size", "destination" } },
    { "structures",
      { "archetype", "name", "pos", "size", "startedAt", "completesAt", "expiresAt", "blueprint",
        "orbits", "grewFrom" },
      { "archetype" } },
};

// One system document, held to the format the loaders read. `gates` says what a gate may
// lead to: null for a system pack, whose gates are those of the system it replaces.
void CheckDocument(const json& doc, const std::string& where,
                   const std::set<std::string>& packFactions, const std::set<std::string>* gates,
                   std::vector<std::string>& problems)
{
    if (!doc.is_object())
    {
        problems.push_back(where + ": a system document is a JSON object");
        return;
    }
    Known(doc,
          { "star", "stars", "planets", "stations", "asteroidFields", "nebulae", "derelicts",
            "gates", "structures", "character" },
          where, problems);
    if (doc.contains("character") && !doc["character"].is_string())
        problems.push_back(where + ": \"character\" is a string");
    if (doc.contains("star"))
    {
        if (!doc["star"].is_object())
            problems.push_back(where + ": \"star\" is an object");
        else
        {
            Known(doc["star"], { "type", "size", "archetype" }, where + ": star", problems);
            if (doc["star"].contains("type"))
                StarKind(doc["star"]["type"].is_string() ? doc["star"]["type"].get<std::string>()
                                                         : std::string(),
                         where + ": star", problems);
        }
    }
    const size_t planets =
        doc.contains("planets") && doc["planets"].is_array() ? doc["planets"].size() : 0;
    const double reach = World::SYSTEM_RADIUS;
    for (const ListRule& rule : LISTS)
    {
        if (!doc.contains(rule.list))
            continue;
        const json& list = doc[rule.list];
        if (!list.is_array())
        {
            problems.push_back(where + ": " + Quoted(rule.list) + " is a list");
            continue;
        }
        if (gates == nullptr && std::string(rule.list) == "gates" && !list.empty())
            problems.push_back(where + ": a system pack has no gates of its own -- it keeps "
                                       "those of the system it takes the place of");
        for (size_t i = 0; i < list.size(); i++)
        {
            const json&       o = list[i];
            const std::string at = where + ": " + rule.list + "[" + std::to_string(i) + "]" +
                                   (o.is_object() && o.contains("name") && o["name"].is_string()
                                        ? " " + Quoted(o["name"].get<std::string>())
                                        : std::string());
            if (!o.is_object())
            {
                problems.push_back(at + " is not an object");
                continue;
            }
            Known(o, rule.known, at, problems);
            for (const char* r : rule.required)
                if (!o.contains(r))
                    problems.push_back(at + ": " + Quoted(r) + " is missing");
            for (const char* text : { "name", "faction", "role", "archetype", "resource",
                                      "destination", "type", "deposit", "blueprint" })
                if (o.contains(text) && !o[text].is_string())
                    problems.push_back(at + ": " + Quoted(text) + " is a string");
            for (const char* number :
                 { "size", "radius", "ore", "reward", "orbitRadius", "orbitSpeed", "angle",
                   "expiresAt", "startedAt", "completesAt" })
                if (o.contains(number) && !o[number].is_number())
                    problems.push_back(at + ": " + Quoted(number) + " is a number");
            if (o.contains("pos"))
            {
                if (!IsPoint(o["pos"]))
                    problems.push_back(at + ": \"pos\" is [x, y]");
                else if (std::sqrt(o["pos"][0].get<double>() * o["pos"][0].get<double>() +
                                   o["pos"][1].get<double>() * o["pos"][1].get<double>()) > reach)
                    problems.push_back(at + ": \"pos\" is outside the system (its radius is " +
                                       std::to_string((int)reach) + ")");
            }
            if (o.contains("orbitRadius") && o["orbitRadius"].is_number() &&
                o["orbitRadius"].get<double>() > reach)
                problems.push_back(at + ": \"orbitRadius\" is outside the system");
            if (o.contains("orbits"))
            {
                const json& orb = o["orbits"];
                if (!orb.is_object() || !orb.contains("planet") ||
                    !orb["planet"].is_number_integer() || !orb.contains("radius") ||
                    !orb["radius"].is_number())
                    problems.push_back(at + ": \"orbits\" is { \"planet\": i, \"radius\": r, "
                                            "\"speed\": s, \"phase\": p }");
                else
                {
                    Known(orb, { "planet", "radius", "speed", "phase" }, at + ": orbits", problems);
                    const int p = orb["planet"].get<int>();
                    if (p < 0 || (size_t)p >= planets)
                        problems.push_back(at + ": orbits planet " + std::to_string(p) +
                                           ", and the system has " + std::to_string(planets));
                }
            }
            if (o.contains("faction") && o["faction"].is_string())
            {
                const std::string f = o["faction"].get<std::string>();
                FactionId         known;
                if (!Factions::Find(f, known) && packFactions.count(f) == 0)
                    problems.push_back(at + ": faction " + Quoted(f) +
                                       " is neither in factions.json nor one of the pack's");
            }
            if (std::string(rule.list) == "stations" && o.contains("role") && o["role"].is_string())
            {
                StationRole role;
                if (!ParseStationRole(o["role"].get<std::string>(), role))
                    problems.push_back(at + ": unknown role " +
                                       Quoted(o["role"].get<std::string>()));
            }
            if (std::string(rule.list) == "planets" && o.contains("type") && o["type"].is_string())
            {
                PlanetType type;
                if (!ParsePlanetType(o["type"].get<std::string>(), type))
                    problems.push_back(at + ": unknown planet type " +
                                       Quoted(o["type"].get<std::string>()));
            }
            if (std::string(rule.list) == "stars" && o.contains("type") && o["type"].is_string())
                StarKind(o["type"].get<std::string>(), at, problems);
            for (const char* res : { "resource", "deposit" })
                if (o.contains(res) && o[res].is_string() && !IsResource(o[res].get<std::string>()))
                    problems.push_back(at + ": " + Quoted(res) +
                                       " is \"Iron\", \"Ice\" or "
                                       "\"Crystal\"");
            if (o.contains("archetype") && o["archetype"].is_string() &&
                Archetypes::Find(o["archetype"].get<std::string>()) == nullptr)
                problems.push_back(at + ": unknown archetype " +
                                   Quoted(o["archetype"].get<std::string>()));
            if (gates != nullptr && std::string(rule.list) == "gates" &&
                o.contains("destination") && o["destination"].is_string() &&
                gates->count(o["destination"].get<std::string>()) == 0)
                problems.push_back(at + ": leads to " +
                                   Quoted(o["destination"].get<std::string>()) +
                                   ", which pack.json does not link this system to");
        }
    }
}

// weave.place, in any of its forms.
void ReadPlace(const json& p, Pack& pack, const std::string& where,
               std::vector<std::string>& problems)
{
    const std::string forms = "\"random\", \"near-centre\", {\"depth\": [a, b]}, {\"slot\": n}" +
                              std::string(pack.kind == "system" ? " or {\"system\": id}" : "");
    if (p.is_string())
    {
        const std::string s = p.get<std::string>();
        if (s == "random" || s == "near-centre")
            pack.place = s;
        else
            problems.push_back(where + ": weave.place " + Quoted(s) + " is not one of " + forms);
        return;
    }
    if (!p.is_object() || p.size() != 1)
    {
        problems.push_back(where + ": weave.place is one of " + forms);
        return;
    }
    if (p.contains("depth"))
    {
        const json& d = p["depth"];
        if (!d.is_array() || d.size() != 2 || !d[0].is_number_integer() ||
            !d[1].is_number_integer() || d[0].get<int>() < 0 || d[1].get<int>() < d[0].get<int>())
        {
            problems.push_back(where + ": weave.place.depth is [least, most] lattice steps from "
                                       "a centre, 0 <= least <= most");
            return;
        }
        pack.place = "depth";
        pack.depthMin = d[0].get<int>();
        pack.depthMax = d[1].get<int>();
    }
    else if (p.contains("slot"))
    {
        if (!p["slot"].is_number_integer() || p["slot"].get<int>() < 0)
        {
            problems.push_back(where + ": weave.place.slot is a slot number, 0 or more");
            return;
        }
        pack.place = "slot";
        pack.slot = p["slot"].get<int>();
    }
    else if (p.contains("system") && pack.kind == "system")
    {
        const std::string id = p["system"].is_string() ? p["system"].get<std::string>() : "";
        if (SlotOf(id) < 0 || DepthOf(id) < 2)
        {
            problems.push_back(where + ": weave.place.system names a generated system beyond "
                                       "the way in, \"r<slot>-<depth>-<k>\" with depth 2 or more");
            return;
        }
        pack.place = "system";
        pack.system = id;
        pack.slot = SlotOf(id);
    }
    else
        problems.push_back(where + ": weave.place is one of " + forms);
}

void ReadWeave(const json& w, Pack& pack, const std::string& where,
               std::vector<std::string>& problems)
{
    if (!w.is_object())
    {
        problems.push_back(where + ": \"weave\" is an object");
        return;
    }
    Known(w, { "place", "links" }, where + ": weave", problems);
    if (w.contains("place"))
        ReadPlace(w["place"], pack, where, problems);
    if (!w.contains("links"))
        return;
    if (pack.kind == "system")
    {
        problems.push_back(where + ": a system pack has no links of its own -- it keeps the "
                                   "gates of the system it takes the place of");
        return;
    }
    const json& l = w["links"];
    if (!l.is_object())
    {
        problems.push_back(where + ": weave.links is { \"count\": [least, most], \"kind\": ... }");
        return;
    }
    Known(l, { "count", "kind" }, where + ": weave.links", problems);
    if (l.contains("count"))
    {
        const json& c = l["count"];
        int         lo = 0, hi = 0;
        if (c.is_number_integer())
            lo = hi = c.get<int>();
        else if (c.is_array() && c.size() == 2 && c[0].is_number_integer() &&
                 c[1].is_number_integer())
        {
            lo = c[0].get<int>();
            hi = c[1].get<int>();
        }
        if (lo < 1 || hi < lo || hi > PACK_MAX_LINKS)
            problems.push_back(where + ": weave.links.count is n or [least, most], 1 to " +
                               std::to_string(PACK_MAX_LINKS) +
                               " -- every system needs a way in and out");
        else
        {
            pack.linksMin = lo;
            pack.linksMax = hi;
        }
    }
    if (l.contains("kind"))
    {
        const std::string k = l["kind"].is_string() ? l["kind"].get<std::string>() : "";
        if (k == "static")
            pack.linkKind = k;
        else if (k == "dynamic" || k == "anchor-required")
            problems.push_back(where + ": weave.links.kind " + Quoted(k) +
                               " is not built yet (roaming holes and mouths come with #342 "
                               "S9) -- use \"static\"");
        else
            problems.push_back(where + ": weave.links.kind is \"static\", \"dynamic\" or "
                                       "\"anchor-required\"");
    }
}

}  // namespace

std::string PackHashText(uint64_t hash)
{
    std::string text(16, '0');
    for (int i = 15; i >= 0; i--, hash >>= 4)
        text[(size_t)i] = "0123456789abcdef"[hash & 0xFu];
    return text;
}

bool PackIdValid(const std::string& id)
{
    return PackIdOk(id);
}

std::string DockingStation(const nlohmann::json& document)
{
    for (const auto& e : WorldLoader::BuildSystem(document))
        if (e->GetKind() == EntityKind::Station && e->Has(Component::Dockable))
            return e->GetName();
    return std::string();
}

bool LoadPack(const std::string& dirIn, Pack& out, std::vector<std::string>& problems)
{
    namespace fs = std::filesystem;
    std::string dir = dirIn;
    while (!dir.empty() && (dir.back() == '/' || dir.back() == '\\'))
        dir.pop_back();
    const size_t      before = problems.size();
    const std::string dirName = fs::path(dir).filename().string();
    std::string       where = dirName + "/pack.json";
    std::ifstream     in(dir + "/pack.json");
    if (!in.is_open())
    {
        problems.push_back(where + ": not found");
        return false;
    }
    const json j = json::parse(in, nullptr, false);
    if (j.is_discarded() || !j.is_object())
    {
        problems.push_back(where + ": not valid JSON");
        return false;
    }
    Pack pack;
    pack.dir = dir;
    Known(j,
          { "schema", "id", "name", "kind", "systems", "links", "factions", "relations", "start",
            "weave", "description" },
          where, problems);
    if (!j.contains("schema") || !j["schema"].is_number_integer())
        problems.push_back(where + ": \"schema\" is missing (this build reads schema " +
                           std::to_string(PACK_SCHEMA) + ")");
    else if (j["schema"].get<int>() != PACK_SCHEMA)
        problems.push_back(where + ": schema " + std::to_string(j["schema"].get<int>()) +
                           " -- this build reads schema " + std::to_string(PACK_SCHEMA));
    pack.id = j.contains("id") && j["id"].is_string() ? j["id"].get<std::string>() : "";
    if (!PackIdOk(pack.id))
        problems.push_back(where + ": \"id\" is lower-case letters, digits, '-' and '_'");
    else if (pack.id != dirName)
        problems.push_back(where + ": \"id\" is " + Quoted(pack.id) +
                           " but the pack's directory is " + Quoted(dirName) +
                           " -- they are the same, so a pack is found by its name");
    else
        where = pack.id + "/pack.json";
    for (const char* text : { "name", "kind", "description" })
        if (j.contains(text) && !j[text].is_string())
            problems.push_back(where + ": " + Quoted(text) + " is a string");
    pack.name = j.value("name", pack.id);
    pack.kind = j.contains("kind") && j["kind"].is_string() ? j["kind"].get<std::string>() : "";
    if (pack.kind != "region" && pack.kind != "system")
        problems.push_back(where + ": \"kind\" is \"region\" or \"system\"");

    // Its factions first: its systems may name them.
    std::set<std::string> own;
    if (j.contains("factions"))
    {
        if (!j["factions"].is_array())
            problems.push_back(where + ": \"factions\" is a list of factions.json's entries");
        else
            for (const json& f : j["factions"])
            {
                FactionDef  d;
                std::string error;
                FactionId   known;
                if (!Factions::ReadDef(f, d, error))
                    problems.push_back(where + ": factions: " + error);
                else if (Factions::Find(d.id, known))
                    problems.push_back(where + ": faction " + Quoted(d.id) +
                                       " is already in the registry -- a pack adds its own");
                else if (!own.insert(d.id).second)
                    problems.push_back(where + ": faction " + Quoted(d.id) + " twice");
                else
                    pack.factions.push_back(f);
            }
    }
    if (j.contains("relations"))
    {
        if (!j["relations"].is_array())
            problems.push_back(where + ": \"relations\" is a list of [a, b, stance]");
        else
            for (const json& r : j["relations"])
            {
                Stance s;
                if (!r.is_array() || r.size() != 3 || !r[0].is_string() || !r[1].is_string() ||
                    !r[2].is_string() || !Factions::ParseStance(r[2].get<std::string>(), s))
                {
                    problems.push_back(where + ": a relation is [a, b, \"War\" | \"Hostile\" | "
                                               "\"Neutral\" | \"Friendly\" | \"Ally\"]");
                    continue;
                }
                bool ours = false, ok = true;
                for (int k = 0; k < 2; k++)
                {
                    const std::string f = r[(size_t)k].get<std::string>();
                    FactionId         known;
                    ours = ours || own.count(f) != 0;
                    if (own.count(f) == 0 && !Factions::Find(f, known))
                    {
                        problems.push_back(where + ": relation names faction " + Quoted(f) +
                                           ", which is not there");
                        ok = false;
                    }
                }
                if (ok && !ours)
                    problems.push_back(where + ": a relation names one of the pack's own "
                                               "factions -- the others' are factions.json's");
                else if (ok)
                    pack.relations.push_back(r);
            }
    }

    // The systems: the index entries, then each document.
    if (!j.contains("systems") || !j["systems"].is_array() || j["systems"].empty())
        problems.push_back(where + ": \"systems\" lists at least one system");
    else if ((int)j["systems"].size() > PACK_MAX_SYSTEMS)
        problems.push_back(where + ": " + std::to_string(j["systems"].size()) +
                           " systems -- a pack has at most " + std::to_string(PACK_MAX_SYSTEMS));
    else if (pack.kind == "system" && j["systems"].size() != 1)
        problems.push_back(where + ": a system pack has exactly one system");
    else
        for (const json& s : j["systems"])
        {
            if (!s.is_object() || !s.contains("id") || !s["id"].is_string() ||
                s["id"].get<std::string>().empty())
            {
                problems.push_back(where + ": every system has an \"id\"");
                continue;
            }
            PackSystem ps;
            ps.id = s["id"].get<std::string>();
            const std::string at = where + ": system " + Quoted(ps.id);
            Known(s, { "id", "file", "map", "name", "security", "owner" }, at, problems);
            for (const PackSystem& other : pack.systems)
                if (other.id == ps.id)
                    problems.push_back(at + " twice");
            if (s.contains("name") && !s["name"].is_string())
                problems.push_back(at + ": \"name\" is a string");
            ps.name = s.contains("name") && s["name"].is_string() ? s["name"].get<std::string>()
                                                                  : std::string();
            if (!s.contains("file") || !s["file"].is_string())
                problems.push_back(at + ": \"file\" is missing");
            else
                ps.file = s["file"].get<std::string>();
            if (s.contains("map"))
            {
                if (!IsPoint(s["map"]))
                    problems.push_back(at + ": \"map\" is [x, y]");
                else
                {
                    ps.mapX = s["map"][0].get<double>();
                    ps.mapY = s["map"][1].get<double>();
                    if (std::fabs(ps.mapX) > PACK_MAP_REACH || std::fabs(ps.mapY) > PACK_MAP_REACH)
                        problems.push_back(at + ": \"map\" is more than " +
                                           std::to_string((int)PACK_MAP_REACH) +
                                           " from the pack's middle -- it would lie in the "
                                           "next slot of the galaxy");
                }
            }
            if (s.contains("security"))
            {
                if (!s["security"].is_number() || s["security"].get<double>() < 0.0 ||
                    s["security"].get<double>() > 1.0)
                    problems.push_back(at + ": \"security\" is 0 to 1");
                else
                    ps.security = s["security"].get<double>();
            }
            if (s.contains("owner"))
            {
                FactionId known;
                ps.owner = s["owner"].is_string() ? s["owner"].get<std::string>() : "";
                if (!ps.owner.empty() && !Factions::Find(ps.owner, known) &&
                    own.count(ps.owner) == 0)
                    problems.push_back(at + ": owner " + Quoted(ps.owner) +
                                       " is neither in factions.json nor one of the pack's");
            }
            if (!ps.file.empty())
            {
                std::ifstream doc(dir + "/" + ps.file);
                if (!doc.is_open())
                    problems.push_back(pack.id + "/" + ps.file + ": not found");
                else
                {
                    ps.document = json::parse(doc, nullptr, false);
                    if (ps.document.is_discarded())
                        problems.push_back(pack.id + "/" + ps.file + ": not valid JSON");
                }
            }
            pack.systems.push_back(std::move(ps));
        }
    std::set<std::string> ids;
    for (const PackSystem& s : pack.systems)
        ids.insert(s.id);

    // Links: between its own systems, each once, and every system reached from the first.
    std::map<std::string, std::set<std::string>> linked;
    if (j.contains("links"))
    {
        if (!j["links"].is_array())
            problems.push_back(where + ": \"links\" is a list of [a, b]");
        else if (pack.kind == "system" && !j["links"].empty())
            problems.push_back(where + ": a system pack has no links of its own -- it keeps the "
                                       "gates of the system it takes the place of");
        else
            for (const json& l : j["links"])
            {
                if (!l.is_array() || l.size() != 2 || !l[0].is_string() || !l[1].is_string())
                {
                    problems.push_back(where + ": a link is [a, b]");
                    continue;
                }
                const std::string a = l[0].get<std::string>(), b = l[1].get<std::string>();
                if (ids.count(a) == 0 || ids.count(b) == 0)
                {
                    problems.push_back(where + ": link [" + a + ", " + b + "] names " +
                                       Quoted(ids.count(a) == 0 ? a : b) +
                                       ", which is not one of the pack's systems -- the way "
                                       "out is woven in (weave.links)");
                    continue;
                }
                if (a == b || linked[a].count(b) != 0)
                {
                    problems.push_back(where + ": link [" + a + ", " + b + "] " +
                                       (a == b ? "goes nowhere" : "twice"));
                    continue;
                }
                linked[a].insert(b);
                linked[b].insert(a);
                pack.links.push_back({ a, b });
            }
    }
    if (pack.kind == "region" && !pack.systems.empty())
    {
        std::set<std::string>    reached = { pack.systems.front().id };
        std::vector<std::string> todo = { pack.systems.front().id };
        while (!todo.empty())
        {
            const std::string at = todo.back();
            todo.pop_back();
            for (const std::string& n : linked[at])
                if (reached.insert(n).second)
                    todo.push_back(n);
        }
        for (const PackSystem& s : pack.systems)
            if (reached.count(s.id) == 0)
                problems.push_back(where + ": system " + Quoted(s.id) + " is not linked to " +
                                   Quoted(pack.systems.front().id) +
                                   " -- every system of a pack is reached through its links");
    }

    // Each document, its gates held to the links.
    for (const PackSystem& s : pack.systems)
    {
        if (s.document.is_null() || s.document.is_discarded())
            continue;
        const std::string at = pack.id + "/" + s.file;
        if (pack.kind == "system")
        {
            CheckDocument(s.document, at, own, nullptr, problems);
            continue;
        }
        CheckDocument(s.document, at, own, &linked[s.id], problems);
        std::set<std::string> gated;
        if (s.document.is_object() && s.document.contains("gates") &&
            s.document["gates"].is_array())
            for (const json& g : s.document["gates"])
                if (g.is_object() && g.contains("destination") && g["destination"].is_string())
                    if (!gated.insert(g["destination"].get<std::string>()).second)
                        problems.push_back(at + ": two gates lead to " +
                                           Quoted(g["destination"].get<std::string>()));
        for (const std::string& n : linked[s.id])
            if (gated.count(n) == 0)
                problems.push_back(at + ": pack.json links it to " + Quoted(n) +
                                   ", and no gate here leads there");
    }

    // Starts: its own systems, each with a station a new pilot can dock at.
    if (j.contains("start"))
    {
        if (!j["start"].is_array())
            problems.push_back(where + ": \"start\" is a list of the pack's systems");
        else
            for (const json& st : j["start"])
            {
                const std::string id = st.is_string() ? st.get<std::string>() : "";
                const PackSystem* s = nullptr;
                for (const PackSystem& c : pack.systems)
                    if (c.id == id)
                        s = &c;
                if (s == nullptr)
                {
                    problems.push_back(where + ": start " + Quoted(id) +
                                       " is not one of the pack's systems");
                    continue;
                }
                pack.starts.push_back(id);
                // Built only once the document is known to be sound: the loader trusts it.
                if (problems.size() == before && DockingStation(s->document).empty())
                    problems.push_back(where + ": start " + Quoted(id) +
                                       " has no station a ship can dock at -- a new pilot "
                                       "starts docked");
            }
    }

    if (j.contains("weave"))
        ReadWeave(j["weave"], pack, where, problems);
    else if (pack.kind == "system")
        pack.place = "random";

    // What it is, hashed: the pack file and each document, as parsed.
    uint64_t h = Fnv(j.dump());
    for (const PackSystem& s : pack.systems)
        h = Fnv(s.file + "\n" + s.document.dump(), h);
    pack.hash = h;
    if (problems.size() != before)
        return false;
    out = std::move(pack);
    return true;
}

std::vector<Pack> LoadPacks(const std::string& packsDir, std::vector<std::string>& problems)
{
    namespace fs = std::filesystem;
    std::vector<Pack> packs;
    std::error_code   ec;
    if (!fs::is_directory(packsDir, ec))
        return packs;
    std::vector<std::string> dirs;
    for (fs::directory_iterator it(packsDir, ec), end; !ec && it != end; it.increment(ec))
        if (it->is_directory(ec) && fs::exists(it->path() / "pack.json", ec))
            dirs.push_back(it->path().generic_string());
    std::sort(dirs.begin(), dirs.end());
    for (const std::string& d : dirs)
    {
        Pack p;
        if (LoadPack(d, p, problems))
            packs.push_back(std::move(p));
    }
    std::sort(packs.begin(), packs.end(), [](const Pack& a, const Pack& b) { return a.id < b.id; });
    return packs;
}

int PlaceRegionPack(const Pack& pack, const GalaxySkeleton& skeleton, const std::set<int>& open,
                    const std::set<int>& taken, std::vector<std::string>& problems)
{
    // A free slot beside an open one: the region is reached through its admin links from
    // the moment the world begins.
    std::vector<int> free;
    for (const Slot& s : skeleton.slots)
    {
        if (open.count(s.index) != 0 || taken.count(s.index) != 0 || s.kind == SlotKind::Centre)
            continue;
        bool beside = false;
        for (int n : s.neighbours)
            beside = beside || open.count(n) != 0;
        if (beside)
            free.push_back(s.index);
    }
    std::vector<int> fit;
    int              nearest = 1 << 20;
    for (int s : free)
        nearest = std::min(nearest, skeleton.slots[(size_t)s].hops);
    for (int s : free)
    {
        const int hops = skeleton.slots[(size_t)s].hops;
        if (pack.place == "random" || (pack.place == "near-centre" && hops == nearest) ||
            (pack.place == "depth" && hops >= pack.depthMin && hops <= pack.depthMax) ||
            (pack.place == "slot" && s == pack.slot))
            fit.push_back(s);
    }
    if (fit.empty())
    {
        std::string why = pack.id + ": no slot for it -- ";
        if (pack.place == "slot")
            why += "slot " + std::to_string(pack.slot) +
                   (pack.slot >= (int)skeleton.slots.size()
                        ? " is beyond the skeleton's " + std::to_string(skeleton.slots.size())
                    : open.count(pack.slot) || taken.count(pack.slot) ? " is taken"
                    : skeleton.slots[(size_t)pack.slot].kind == SlotKind::Centre
                        ? " is a civilisation centre"
                        : " is not beside a region the world begins with");
        else if (pack.place == "depth")
            why += "no free slot beside an open region is " + std::to_string(pack.depthMin) +
                   " to " + std::to_string(pack.depthMax) + " steps from a centre";
        else
            why += "every slot beside an open region is taken";
        problems.push_back(why);
        return -1;
    }
    Rng rng(Key(skeleton.seed, PACK_PLACE, IdKey(pack)));
    return fit[(size_t)rng.Range(0, (int)fit.size() - 1)];
}

int PlaceSystemPack(const Pack& pack, const GalaxySkeleton& skeleton,
                    const std::vector<int>& candidates, std::vector<std::string>& problems)
{
    if (pack.place == "slot" || pack.place == "system")
    {
        // Named outright: a frontier slot, open or not -- it takes the system when it opens.
        if (pack.slot >= 0 && pack.slot < (int)skeleton.slots.size() &&
            skeleton.slots[(size_t)pack.slot].kind == SlotKind::Frontier)
            return pack.slot;
        problems.push_back(pack.id + ": slot " + std::to_string(pack.slot) + " is " +
                           (pack.slot >= (int)skeleton.slots.size() ? "beyond the skeleton"
                                                                    : "a civilisation centre") +
                           " -- a system pack goes into a frontier region");
        return -1;
    }
    std::vector<int> fit;
    int              nearest = 1 << 20;
    for (int s : candidates)
        nearest = std::min(nearest, skeleton.slots[(size_t)s].hops);
    for (int s : candidates)
    {
        const int hops = skeleton.slots[(size_t)s].hops;
        if (pack.place == "random" || (pack.place == "near-centre" && hops == nearest) ||
            (pack.place == "depth" && hops >= pack.depthMin && hops <= pack.depthMax))
            fit.push_back(s);
    }
    if (fit.empty())
    {
        problems.push_back(pack.id + ": no frontier region the world begins with matches "
                                     "weave.place");
        return -1;
    }
    std::sort(fit.begin(), fit.end());
    Rng rng(Key(skeleton.seed, PACK_PLACE, IdKey(pack)));
    return fit[(size_t)rng.Range(0, (int)fit.size() - 1)];
}

Region WeaveRegionPack(const Pack& pack, const GalaxySkeleton& skeleton, int slot,
                       const std::vector<Region>& neighbours, std::vector<std::string>& problems)
{
    Region region;
    region.slot = slot;
    region.pack = pack.id;
    double cx = 0.0, cy = 0.0;
    SlotMapPoint(skeleton.seed, slot, cx, cy);

    // Depth along its links from its first system, which is the way in (ring 1); each ring
    // numbered in the order the pack lists its systems.
    std::map<std::string, int> depth;
    depth[pack.systems.front().id] = 1;
    for (bool grew = true; grew;)
    {
        grew = false;
        for (const auto& l : pack.links)
            for (int side = 0; side < 2; side++)
            {
                const std::string& a = side == 0 ? l.first : l.second;
                const std::string& b = side == 0 ? l.second : l.first;
                if (depth.count(a) != 0 && (depth.count(b) == 0 || depth[b] > depth[a] + 1))
                {
                    depth[b] = depth[a] + 1;
                    grew = true;
                }
            }
    }
    std::map<std::string, std::string> idOf, nameOf;
    std::map<int, int>                 inRing;
    for (const PackSystem& s : pack.systems)
    {
        const int d = depth.count(s.id) != 0 ? depth[s.id] : 1;
        const int k = ++inRing[d];
        idOf[s.id] = SystemId(slot, d, k);
        // Nameless, it is designated as a generated system of the slot is, until named (S8).
        nameOf[s.id] = s.name.empty() ? SystemDesignation(slot, d, k) : s.name;
    }
    region.entryId = idOf[pack.systems.front().id];

    std::map<std::string, std::pair<double, double>> mapOf;
    for (const PackSystem& s : pack.systems)
    {
        const std::string id = idOf[s.id];
        const double      mx = std::floor(cx + s.mapX + 0.5), my = std::floor(cy + s.mapY + 0.5);
        mapOf[id] = { mx, my };
        region.systems.push_back({ { "id", id },
                                   { "name", nameOf[s.id] },
                                   { "map", json::array({ mx, my }) },
                                   { "security", s.security },
                                   { "owner", s.owner } });
        json doc = s.document;
        if (doc.contains("gates"))
            for (json& g : doc["gates"])
                g["destination"] = idOf[g["destination"].get<std::string>()];
        region.documents[id] = doc;
    }
    for (const auto& l : pack.links)
        region.links.push_back(json::array({ idOf[l.first], idOf[l.second] }));
    for (const std::string& s : pack.starts)
        region.starts.push_back(idOf[s]);
    region.factions = pack.factions;
    region.relations = pack.relations;

    // The admin links: to systems of the open regions beside it, spread over them.
    std::vector<const Region*> beside;
    for (const Region& n : neighbours)
        if (!n.systems.empty())
            beside.push_back(&n);
    std::sort(beside.begin(), beside.end(),
              [](const Region* a, const Region* b) { return a->slot < b->slot; });
    if (beside.empty())
    {
        problems.push_back(pack.id + ": slot " + std::to_string(slot) +
                           " has no open region beside it to link to");
        return region;
    }
    Rng                                           rng(Key(skeleton.seed, PACK_LINKS, IdKey(pack)));
    const int                                     count = rng.Range(pack.linksMin, pack.linksMax);
    const int                                     first = rng.Range(0, (int)beside.size() - 1);
    std::set<std::pair<std::string, std::string>> made;
    std::set<std::string>                         usedOutside;
    for (int i = 0; i < count; i++)
    {
        const Region&            n = *beside[(size_t)((first + i) % (int)beside.size())];
        std::vector<std::string> outside;
        for (const json& s : n.systems)
        {
            const std::string id = s.value("id", std::string());
            if (!id.empty() && usedOutside.count(id) == 0 && s.contains("map"))
                outside.push_back(id);
        }
        std::sort(outside.begin(), outside.end());
        if (outside.empty())
            continue;
        const std::string there = outside[(size_t)rng.Range(0, (int)outside.size() - 1)];
        const std::string here =
            idOf[pack.systems[(size_t)rng.Range(0, (int)pack.systems.size() - 1)].id];
        if (!made.insert({ here, there }).second)
            continue;
        usedOutside.insert(there);
        double      tx = 0.0, ty = 0.0;
        std::string thereName = there;
        for (const json& s : n.systems)
            if (s.value("id", std::string()) == there)
            {
                tx = s["map"][0].get<double>();
                ty = s["map"][1].get<double>();
                thereName = s.value("name", there);
            }
        std::string hereName = here;
        for (const json& s : region.systems)
            if (s["id"] == here)
                hereName = s["name"].get<std::string>();
        const double hx = mapOf[here].first, hy = mapOf[here].second;
        json&        hereDoc = region.documents[here];
        if (!hereDoc.contains("gates"))
            hereDoc["gates"] = json::array();
        hereDoc["gates"].push_back(
            { { "name", "Gate to " + thereName },
              { "pos", EdgeGatePos(Key(skeleton.seed, PACK_GATE, IdKey(pack), (uint64_t)(2 * i)),
                                   &hereDoc, tx - hx, ty - hy) },
              { "size", 900 },
              { "destination", there } });
        const auto  thereDoc = n.documents.find(there);
        const json* td = thereDoc != n.documents.end() ? &thereDoc->second : nullptr;
        region.crossings.push_back(
            { { "home", there },
              { "gate",
                { { "name", "Gate to " + hereName },
                  { "pos",
                    EdgeGatePos(Key(skeleton.seed, PACK_GATE, IdKey(pack), (uint64_t)(2 * i + 1)),
                                td, hx - tx, hy - ty) },
                  { "size", 900 },
                  { "destination", here } } } });
        region.links.push_back(json::array({ here, there }));
    }
    return region;
}

std::string SystemPackTarget(const Pack& pack, const Region& region, uint64_t seed,
                             const std::string& wanted)
{
    std::vector<std::string> fit;
    for (const json& s : region.systems)
    {
        const std::string id = s.value("id", std::string());
        if (id.empty() || id == region.entryId || id == region.heartId || DepthOf(id) < 2)
            continue;
        if (!wanted.empty())
        {
            if (id == wanted)
                return id;
            continue;
        }
        // Not anybody's station, and not where a way out of the region is.
        const auto doc = region.documents.find(id);
        if (doc == region.documents.end())
            continue;
        bool taken = doc->second.contains("stations") && !doc->second["stations"].empty();
        if (doc->second.contains("gates"))
            for (const json& g : doc->second["gates"])
                taken = taken || SlotOf(g.value("destination", std::string())) != region.slot;
        if (!taken)
            fit.push_back(id);
    }
    if (fit.empty())
        return std::string();
    std::sort(fit.begin(), fit.end());
    Rng rng(Key(seed, PACK_SYSTEM, IdKey(pack), (uint64_t)region.slot));
    return fit[(size_t)rng.Range(0, (int)fit.size() - 1)];
}

bool ApplySystemPack(const Pack& pack, Region& region, const std::string& system,
                     std::vector<std::string>& problems)
{
    const PackSystem& s = pack.systems.front();
    // A pin's replace: the document is the system, and its gates are the generated ones.
    json         pins = { { "pins", json::array({ { { "system", system },
                                                    { "slot", region.slot },
                                                    { "mode", "replace" },
                                                    { "document", s.document } } }) } };
    const size_t before = problems.size();
    ApplyPins(region, pins, 0, problems);
    for (size_t i = before; i < problems.size(); i++)
        problems[i] = pack.id + ": " + problems[i];
    if (problems.size() != before)
        return false;
    for (json& e : region.systems)
        if (e.value("id", std::string()) == system)
        {
            if (!s.name.empty())  // nameless, it keeps the designation of the one it replaces
                e["name"] = s.name;
            e["security"] = s.security;
            e["owner"] = s.owner;
        }
    if (!pack.starts.empty())
        region.starts.push_back(system);
    for (const json& f : pack.factions)
        region.factions.push_back(f);
    for (const json& r : pack.relations)
        region.relations.push_back(r);
    return true;
}

void RegisterPackFactions(const nlohmann::json& factions, const nlohmann::json& relations)
{
    if (factions.is_array())
        for (const json& f : factions)
        {
            FactionDef  d;
            std::string error;
            if (Factions::ReadDef(f, d, error))
                Factions::Add(d);
        }
    if (relations.is_array())
        for (const json& r : relations)
        {
            FactionId a, b;
            Stance    s;
            if (r.is_array() && r.size() == 3 && r[0].is_string() && r[1].is_string() &&
                r[2].is_string() && Factions::Find(r[0].get<std::string>(), a) &&
                Factions::Find(r[1].get<std::string>(), b) &&
                Factions::ParseStance(r[2].get<std::string>(), s))
                Factions::SetRelation(a, b, s);
        }
}

}  // namespace Gen
