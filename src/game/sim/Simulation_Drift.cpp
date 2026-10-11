// The archive of dead regions (#342 S4, the owner's idea).
//
// At the cap a world cannot grow, so it lets go of what nobody uses: a frontier region nobody
// has been in for days, with no player structure standing in it, drifts away. The chronicle
// warns before; then the region is taken out of the galaxy and kept -- its documents as they
// were sealed, what stood and lay in each of its systems, the names players gave them and its
// history -- and its slot is free. When a slot opens, the tide may bring an archived region
// back instead of a fresh one, possibly into another slot, since it drifted: renamed for its
// new place, and decayed. A faction's structures are ruins, a station is a derelict hulk,
// wrecks lie where they lay and no longer rot, and the names players gave stay.
//
// Everything is decided in the coarse pass, by the clock and the log; which archive comes
// back is drawn from where and how many openings in, never from when.
//
// One translation unit of Simulation (#17).

#include "sim/Simulation.h"
#include "sim/SaveFile.h"
#include "sim/WorldRules.h"

#include "core/Blueprint.h"
#include "economy/Market.h"
#include "entities/Structure.h"
#include "gen/Galaxy.h"
#include "gen/Region.h"
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>

namespace
{
constexpr int    ARCHIVE_SCHEMA = 1;
constexpr double DAY = 86400.0;
// What a station's hulk holds for whoever searches it first.
constexpr double HULK_REWARD = 2500.0;

using nlohmann::json;

// "3 days", or "20 h" for less than two of them.
std::string Span(double seconds)
{
    seconds = std::max(0.0, seconds);
    return seconds >= 2.0 * DAY ? std::to_string((int)(seconds / DAY)) + " days"
                                : std::to_string((int)std::ceil(seconds / 3600.0)) + " h";
}

// Every occurrence of `from` in `s` replaced by `to`.
std::string ReplaceAll(std::string s, const std::string& from, const std::string& to)
{
    if (from.empty() || from == to)
        return s;
    for (size_t at = s.find(from); at != std::string::npos; at = s.find(from, at + to.size()))
        s.replace(at, from.size(), to);
    return s;
}

// What a faction's structure is worth as a ruin: the rules' share of what it cost, at base
// prices -- what HitStructure leaves in a wreck.
double RuinWorth(const json& object)
{
    const Blueprint* bp = Blueprints::Find(object.value("blueprint", std::string()));
    if (bp == nullptr)
        bp = Blueprints::Building(object.value("archetype", std::string()));
    if (bp == nullptr)
        return 0.0;
    const Market prices;
    double       worth = 0.0;
    for (const auto& c : bp->cost)
        worth += c.second * prices.GetBasePrice(c.first);
    return std::floor(worth * Blueprints::Rules().wreckShare);
}

// A station as the derelict it has become: where it stood, and what is left to search.
json Hulk(const json& station)
{
    json h = { { "name", station.value("name", std::string("Station")) + " Hulk" },
               { "size", station.value("size", 300.0) },
               { "reward", HULK_REWARD },
               { "archetype", "derelict.station_hulk" } };
    for (const char* key : { "pos", "orbits" })
        if (station.contains(key))
            h[key] = station[key];
    return h;
}

// What a system's changes are once it has lain in the dark: a faction's structure is a ruin
// (a site, never finished, is nothing), a station's state is a hulk's, and wrecks no longer
// rot. Everything else -- a player's, a belt mined out, a wreck searched -- as it was.
void Decay(json& c)
{
    if (c.contains("added") && c["added"].is_array())
    {
        json kept = json::array();
        for (json& a : c["added"])
        {
            if (!a.is_object() || !a.contains("object") || !a["object"].is_object())
                continue;
            json&             object = a["object"];
            const std::string owner = a.value("owner", std::string());
            FactionId         f;
            if (a.value("array", std::string()) == "structures" && Outposts::FactionOf(owner, f))
            {
                if (object.contains("completesAt"))
                    continue;
                json ruin = { { "name", "Ruin of " + object.value("name", std::string("?")) },
                              { "size", object.value("size", 160.0) },
                              { "reward", RuinWorth(object) },
                              { "archetype", "derelict.outpost_ruin" } };
                for (const char* key : { "pos", "orbits" })
                    if (object.contains(key))
                        ruin[key] = object[key];
                json r = { { "key", a.value("key", std::string()) },
                           { "array", "derelicts" },
                           { "object", std::move(ruin) } };
                kept.push_back(std::move(r));
                continue;
            }
            if (a.value("array", std::string()) == "derelicts")
                object.erase("expiresAt");
            kept.push_back(std::move(a));
        }
        c["added"] = std::move(kept);
    }
    if (c.contains("state") && c["state"].is_object())
    {
        json kept = json::object();
        for (auto it = c["state"].begin(); it != c["state"].end(); ++it)
            if (it.key().rfind("stations/", 0) != 0)
                kept[it.key()] = it.value();
        c["state"] = std::move(kept);
    }
    // A station taken away stays gone: its hulk is not there either.
    if (c.contains("removed") && c["removed"].is_array())
        for (json& k : c["removed"])
            if (k.is_string() && k.get<std::string>().rfind("stations/", 0) == 0)
            {
                const std::string key = k.get<std::string>();
                const size_t      hash = key.rfind('#');
                k = "derelicts/" + key.substr(9, hash - 9) + " Hulk" +
                    (hash == std::string::npos ? std::string() : key.substr(hash));
            }
}
}  // namespace

std::string Simulation::ArchivePath(const std::string& dir, int id)
{
    std::string d = dir;
    if (!d.empty() && d.back() != '/' && d.back() != '\\')
        d += '/';
    return d + "archive-" + std::to_string(id) + ".json";
}

int Simulation::PlayerStructuresIn(int slot) const
{
    int built = 0;
    for (const std::string& id : RegionSystems(slot))
        if (const SystemState* st = SystemById(id))
            for (const auto& e : st->entities)
            {
                FactionId f;
                if (e->GetKind() == EntityKind::Structure && !e->GetOwner().empty() &&
                    !Outposts::FactionOf(e->GetOwner(), f))
                    built++;
            }
    return built;
}

void Simulation::StepDrift()
{
    if (!AtCap())
        return;
    const WorldRules::Archive& r = WorldRules::ArchiveRules();
    const double               dead = r.deadDays * DAY;
    const double               warn = std::min(r.warnDays, r.deadDays) * DAY;
    // Oldest visit first, then the lower slot: one region drifts away a period, and the slot
    // it frees is the tide's.
    RegionOpening* gone = nullptr;
    for (RegionOpening& o : openings_)
    {
        if (o.parent < 0 || o.state == "archived" || o.state == "returned" ||
            OpeningOf(o.slot) != &o)
            continue;
        bool parent = false;
        for (const RegionOpening& c : openings_)
            parent = parent || (c.parent == o.slot && OpeningOf(c.slot) == &c);
        const double idle = time_ - o.lastVisit;
        if (parent || idle < dead - warn || PlayerStructuresIn(o.slot) > 0)
            continue;
        if (!o.fading)
        {
            o.fading = true;
            Announce("drift", std::nullopt, o.mouth,
                     "The region beyond " + SystemName(o.mouth) +
                         " is drifting away: nobody has come to it in " + Span(idle) +
                         ", and unless someone does it is lost within " + Span(dead - idle));
        }
        if (idle >= dead && (gone == nullptr || o.lastVisit < gone->lastVisit))
            gone = &o;
    }
    std::string why;
    if (gone != nullptr && !ArchiveRegion(gone->slot, why))
        TraceLog(LOG_WARNING, "World: region %d could not drift away: %s", gone->slot, why.c_str());
}

int Simulation::ArchiveRegionNow(int slot, std::string& why)
{
    why.clear();
    const int id = nextArchive_;
    return ArchiveRegion(slot, why) ? id : -1;
}

bool Simulation::ArchiveRegion(int slot, std::string& why)
{
    RegionOpening* o = LiveOpening(slot);
    if (o == nullptr || o->parent < 0)
    {
        why = "region " + std::to_string(slot) + " is not an open frontier";
        return false;
    }
    for (const RegionOpening& c : openings_)
        if (c.parent == slot && OpeningOf(c.slot) == &c)
        {
            why = "region " + std::to_string(c.slot) + " opened from it";
            return false;
        }
    if (PlayerStructuresIn(slot) > 0)
    {
        why = "a player's structure stands in it";
        return false;
    }
    const std::vector<std::string> ids = RegionSystems(slot);
    const std::set<std::string>    in(ids.begin(), ids.end());
    for (const auto& kv : sessions_)
        if (in.count(kv.second.systemId) != 0)
        {
            why = "a pilot is in it";
            return false;
        }
    const std::string entry = EntryOf(slot);

    // The region as it is: its documents never change once made, so they are what was sealed.
    Gen::Region r;
    r.slot = slot;
    r.homeId = o->mouth;
    r.entryId = entry;
    for (const auto& info : universe_.systems)
        if (in.count(info.id) != 0)
            r.systems.push_back({ { "id", info.id },
                                  { "name", info.name },
                                  { "map", json::array({ info.mapPos.x, info.mapPos.y }) },
                                  { "security", info.security },
                                  { "owner", info.owner } });
    for (const auto& l : universe_.links)
        if (in.count(l.a) != 0 && in.count(l.b) != 0)
            r.links.push_back(json::array({ l.a, l.b }));
    r.links.push_back(json::array({ o->mouth, entry }));  // the way in, cut or not
    for (const std::string& id : ids)
    {
        const auto doc = regionDocs_.find(id);
        if (doc != regionDocs_.end())
            r.documents[id] = json::parse(doc->second);
    }
    const auto mouths = wormholes_.find(o->mouth);
    if (mouths != wormholes_.end())
        for (const std::string& text : mouths->second)
        {
            const json g = json::parse(text, nullptr, false);
            if (g.is_object() && g.value("destination", std::string()) == entry)
                r.wormhole = g;
        }

    // ...and what had become of it.
    json systems = json::object();
    for (const std::string& id : ids)
        if (const SystemState* st = SystemById(id))
            systems[id] = { { "name", st->agg.givenName },
                            { "discoverer", st->agg.discoverer },
                            { "visited", st->agg.visited },
                            { "changes", ChangesOf(*st) } };
    json history = json::array();
    for (const ChronicleEntry& e : chronicle_)
        if (in.count(e.system) != 0)
            history.push_back({ { "time", e.time },
                                { "kind", e.kind },
                                { "system", e.system },
                                { "text", e.text } });
    const WorldLoader::SystemInfo* home = SystemInfoById(o->mouth);
    const int                      id = nextArchive_++;
    const json     doc = { { "schema", ARCHIVE_SCHEMA },
                           { "seed", regionSeed_ },
                           { "id", id },
                           { "slot", slot },
                           { "parent", o->parent },
                           { "mouth", o->mouth },
                           { "openedAt", o->openedAt },
                           { "archivedAt", time_ },
                           { "homeMap", json::array({ home != nullptr ? home->mapPos.x : 0.0f,
                                                      home != nullptr ? home->mapPos.y : 0.0f }) },
                           { "region", Gen::RegionToJson(r) },
                           { "systems", std::move(systems) },
                           { "history", std::move(history) } };
    ArchivedRegion a;
    a.id = id;
    a.slot = slot;
    a.archivedAt = time_;
    a.text = doc.dump();
    if (!sealDir_.empty())
    {
        std::error_code ec;
        std::filesystem::create_directories(sealDir_, ec);
        if (!Save::WriteFileAtomic(ArchivePath(sealDir_, id), doc.dump(1) + "\n", 0))
        {
            why = "its archive could not be written";
            nextArchive_--;
            return false;
        }
    }
    archives_[id] = std::move(a);

    // Out of the galaxy: the way in, its systems and everything that knew of them.
    if (o->state != "drifted")
        CutMouth(*o);
    const std::string mouth = o->mouth;
    auto&             index = universe_.systems;
    index.erase(std::remove_if(index.begin(), index.end(), [&](const WorldLoader::SystemInfo& s)
                               { return in.count(s.id) != 0; }),
                index.end());
    auto& links = universe_.links;
    links.erase(std::remove_if(links.begin(), links.end(), [&](const WorldLoader::SystemLink& l)
                               { return in.count(l.a) != 0 || in.count(l.b) != 0; }),
                links.end());
    IndexLinks();
    if (mouths != wormholes_.end())
    {
        auto& v = mouths->second;
        v.erase(std::remove_if(v.begin(), v.end(),
                               [&](const std::string& text)
                               {
                                   const json g = json::parse(text, nullptr, false);
                                   return g.is_object() &&
                                          g.value("destination", std::string()) == entry;
                               }),
                v.end());
    }
    for (const std::string& sid : ids)
    {
        regionDocs_.erase(sid);
        wormholes_.erase(sid);
        systems_.erase(sid);
        explored_.erase(sid);
        surveyed_.erase(sid);
    }
    for (FactionId f : Factions::All())
    {
        FactionMind& m = minds_[f];
        for (const std::string& sid : ids)
        {
            m.intel.erase(sid);
            m.lostAt.erase(sid);
        }
        for (auto it = m.unsupplied.begin(); it != m.unsupplied.end();)
            it = in.count(it->substr(0, it->find('/'))) != 0 ? m.unsupplied.erase(it)
                                                             : std::next(it);
    }
    for (auto it = plans_.begin(); it != plans_.end();)
        it = in.count(it->second.target) != 0 || in.count(it->second.from) != 0 ? plans_.erase(it)
                                                                                : std::next(it);
    for (auto it = migrants_.begin(); it != migrants_.end();)
        it = in.count(it->second.from) != 0 || in.count(it->second.to) != 0 ? migrants_.erase(it)
                                                                            : std::next(it);
    tides_.erase(slot);
    playersSeen_.erase(slot);
    openSlots_.erase(slot);
    o = LiveOpening(slot);
    o->state = "archived";
    o->archive = id;
    o->closesAt = 0.0;
    o->fading = false;
    if (!sealDir_.empty())
        std::remove(SealedPath(sealDir_, slot).c_str());  // the slot's file is the next region's
    topologyChanged_ = true;
    chartsChanged_ = true;
    Announce("drift", std::nullopt, mouth,
             "The region beyond " + SystemName(mouth) +
                 " has drifted away into the dark; its place on the map is free");
    return true;
}

int Simulation::BringBack(int id, int slot, int fromSlot, const std::string& by,
                          const std::string& why)
{
    const auto a = archives_.find(id);
    if (a == archives_.end() || openSlots_.count(slot) != 0 || dataDir_.empty())
        return -1;
    const json  doc = json::parse(a->second.text, nullptr, false);
    Gen::Region old;
    if (!doc.is_object() || !doc.contains("region") || !Gen::RegionFromJson(doc["region"], old))
        return -1;
    const int oldSlot = doc.value("slot", old.slot);

    // Its new home: where a mouth from `fromSlot` toward the slot opens.
    double tx, ty;
    Gen::SlotMapPoint(regionSeed_, slot, tx, ty);
    const std::string              home = Gen::MouthSystem(RegionView(fromSlot), tx, ty);
    const WorldLoader::SystemInfo* homeInfo = SystemInfoById(home);
    if (homeInfo == nullptr)
        return -1;

    // Renamed for the slot it is in now: its ids, the designations its names carry, and home.
    std::map<std::string, std::string> ids;
    for (const json& s : old.systems)
    {
        const std::string was = s.value("id", std::string());
        ids[was] = "r" + std::to_string(slot) + was.substr(was.find('-'));
    }
    ids[old.homeId] = home;
    const std::string oldTag = oldSlot == 0 ? "W-" : "R" + std::to_string(oldSlot) + " W-";
    const std::string newTag = slot == 0 ? "W-" : "R" + std::to_string(slot) + " W-";
    auto              text = [&](const std::string& s)
    {
        const auto id = ids.find(s);
        return id != ids.end() ? id->second : ReplaceAll(s, oldTag, newTag);
    };
    std::function<void(json&)> rename = [&](json& j)
    {
        if (j.is_string())
            j = text(j.get<std::string>());
        else if (j.is_array())
            for (json& e : j)
                rename(e);
        else if (j.is_object())
        {
            json renamed = json::object();
            for (auto it = j.begin(); it != j.end(); ++it)
            {
                json v = it.value();
                rename(v);
                renamed[text(it.key())] = std::move(v);
            }
            j = std::move(renamed);
        }
    };

    Gen::Region r;
    r.slot = slot;
    r.homeId = home;
    r.entryId = text(old.entryId);
    // Moved on the map by as much as its home moved: it drifted, and it comes back whole.
    const json  homeMap = doc.value("homeMap", json::array({ 0.0, 0.0 }));
    const float dx = homeInfo->mapPos.x - homeMap[0].get<float>();
    const float dy = homeInfo->mapPos.y - homeMap[1].get<float>();
    for (const json& s : old.systems)
    {
        json e = s;
        rename(e);
        e["map"] = json::array({ e["map"][0].get<float>() + dx, e["map"][1].get<float>() + dy });
        r.systems.push_back(std::move(e));
    }
    for (const json& l : old.links)
    {
        json e = l;
        rename(e);
        r.links.push_back(std::move(e));
    }
    for (const auto& kv : old.documents)
    {
        json d = kv.second;
        rename(d);
        // A station nobody kept is a hulk now.
        if (d.contains("stations") && d["stations"].is_array())
        {
            for (const json& s : d["stations"])
                d["derelicts"].push_back(Hulk(s));
            d.erase("stations");
        }
        r.documents[text(kv.first)] = std::move(d);
    }
    // Its mouth where the new home has room for it: where the slot's own region would have
    // put one, from there.
    Gen::RegionParams params;
    json              homeDoc;
    if (!ParamsFor(slot, home, dataDir_ + "systems/", params, homeDoc))
        return -1;
    const Gen::Region placed = Gen::GenerateRegion(params);
    r.wormhole = { { "name", "Wormhole" },
                   { "pos", placed.wormhole.value("pos", json::array({ 900000, 0 })) },
                   { "size", placed.wormhole.value("size", 900) },
                   { "destination", r.entryId } };

    // What had become of each system, decayed, under its new name.
    returnState_.clear();
    std::vector<std::string> named;
    const json&              systems = doc.value("systems", json::object());
    for (auto it = systems.begin(); systems.is_object() && it != systems.end(); ++it)
    {
        json s = it.value();
        json c = s.value("changes", json());
        if (c.is_object())
        {
            rename(c);
            Decay(c);
        }
        s["changes"] = std::move(c);
        if (!s.value("name", std::string()).empty())
            named.push_back(s.value("name", std::string()));
        returnState_[text(it.key())] = s.dump();
    }

    returning_ = &r;
    const bool attached = AttachRegion(regionSeed_, slot, home, dataDir_ + "systems/");
    returning_ = nullptr;
    if (!attached)
    {
        returnState_.clear();
        return -1;
    }
    RegionOpening& o = openings_.back();
    o.by = by;
    o.why = why;
    o.archive = id;
    if (WorldRules::MouthRules().anchors)
    {
        o.state = "unstable";
        o.closesAt = time_ + WorldRules::TideRules().mouthHours * 3600.0;
    }
    MaterializeOpened(slot);
    for (RegionOpening& e : openings_)
        if (e.state == "archived" && e.archive == id)
            e.state = "returned";
    archives_.erase(a);
    if (!sealDir_.empty())
        std::remove(ArchivePath(sealDir_, id).c_str());
    topologyChanged_ = true;

    std::string names;
    for (const std::string& n : named)
        names += (names.empty() ? "" : ", ") + n;
    const double lost = time_ - doc.value("archivedAt", time_);
    Announce("drift", std::nullopt, home,
             "Out of the dark beyond " + SystemName(home) + " a lost region drifted back, gone " +
                 Span(lost) + (names.empty() ? std::string() : " -- " + names + " among it") +
                 "; what stood in it is ruins now");
    return slot;
}

void Simulation::SaveArchive(json& j) const
{
    json list = json::array();
    for (const auto& kv : archives_)
    {
        json e = { { "id", kv.second.id },
                   { "slot", kv.second.slot },
                   { "archivedAt", kv.second.archivedAt } };
        // Beside the sealed regions, when there is somewhere to keep them; in here when not.
        if (sealDir_.empty())
            e["doc"] = json::parse(kv.second.text);
        list.push_back(std::move(e));
    }
    j["archive"] = { { "next", nextArchive_ }, { "regions", std::move(list) } };
}

void Simulation::LoadArchive(const json& j)
{
    archives_.clear();
    nextArchive_ = 1;
    if (!j.contains("archive") || !j["archive"].is_object())
        return;
    const json& a = j["archive"];
    nextArchive_ = std::max(1, a.value("next", 1));
    for (const json& e : a.value("regions", json::array()))
    {
        if (!e.is_object() || !e.contains("id") || !e["id"].is_number_integer())
            continue;
        ArchivedRegion r;
        r.id = e["id"].get<int>();
        r.slot = e.value("slot", 0);
        r.archivedAt = e.value("archivedAt", 0.0);
        if (e.contains("doc") && e["doc"].is_object())
            r.text = e["doc"].dump();
        else if (!sealDir_.empty())
        {
            std::ifstream in(ArchivePath(sealDir_, r.id));
            if (in.is_open())
                r.text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }
        const json check = json::parse(r.text, nullptr, false);
        if (check.is_discarded() || check.value("seed", (uint64_t)0) != regionSeed_)
        {
            TraceLog(LOG_WARNING, "World: archived region %d is missing or another world's -- lost",
                     r.id);
            continue;
        }
        r.text = check.dump();  // as it is kept in memory, whatever the file's layout
        nextArchive_ = std::max(nextArchive_, r.id + 1);
        archives_[r.id] = std::move(r);
    }
}
