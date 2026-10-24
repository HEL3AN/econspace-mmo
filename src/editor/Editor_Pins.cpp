// Editing a generated system (#237). The generator is the default and a pin is the
// exception (#147), so the editor opens the system the server would build -- generated,
// then pinned -- lets the ordinary tools work on it, and saves only what was changed, as
// a pin. One translation unit of Editor (#17); the difference itself is Gen::PinFor, in
// the engine, where a test holds it to making the same system again.

#include "Editor.h"

#include "core/Faction.h"
#include "gen/Region.h"

#include <fstream>
#include <string>

using nlohmann::json;

json Editor::ReadPins(const std::string& path)
{
    std::ifstream in(path);
    if (!in.is_open())
        return json::object();
    json pins = json::parse(in, nullptr, false);
    return pins.is_discarded() ? json() : pins;
}

bool Editor::OpenGenerated(uint64_t seed, const std::string& systemId, bool everySeed)
{
    std::string                        id = systemId;
    Gen::Region                        region;
    std::map<std::string, std::string> names;
    if (!WorldRegion(seed, id, region, &names))
    {
        std::string ids;
        for (const json& s : region.systems)
            ids += " " + s.value("id", std::string());
        TraceLog(LOG_WARNING, "Editor: seed %llu makes no system \"%s\"; its region has:%s",
                 (unsigned long long)seed, id.c_str(), ids.c_str());
        return false;
    }
    const json pins = ReadPins(dataDir_ + "pins.json");
    if (pins.is_null())
    {
        // Saving over a file that could not be read would lose every pin in it.
        TraceLog(LOG_WARNING, "Editor: %spins.json is not valid JSON -- not opening",
                 dataDir_.c_str());
        return false;
    }

    Gen::EditedSystem es;
    Gen::OpenForEditing(region, pins, seed, id, es, everySeed);
    for (const std::string& p : es.problems)
        TraceLog(LOG_WARNING, "Pins: %s -- not applied", p.c_str());

    generated_ = GeneratedEdit();
    generated_.open = true;
    generated_.seed = seed;
    generated_.everySeed = everySeed;
    generated_.id = id;
    generated_.name = id;
    for (const json& s : region.systems)
        if (s.value("id", std::string()) == id)
            generated_.name = s.value("name", id);
    generated_.base = es.base;
    generated_.origins = es.origins;
    generated_.problems = (int)es.problems.size();
    generated_.systems = Gen::Destinations(region);
    generated_.names = names;
    for (const json& s : region.systems)
        generated_.names[s.value("id", std::string())] = s.value("name", std::string());

    mode_ = Mode::System;
    selected_ = -1;
    dirty_ = false;
    activeField_.clear();
    openDropdown_.clear();
    placeArchetype_.clear();
    systemJson_ = es.edited;
    camera_.target = { 0.0f, 0.0f };
    camera_.zoom = 0.00035f;  // the whole system
    RebuildEntities();
    TraceLog(LOG_INFO, "Editor: opened generated system %s (%s) of seed %llu%s", id.c_str(),
             generated_.name.c_str(), (unsigned long long)seed,
             everySeed ? ", editing its pins for every seed" : "");
    return true;
}

void Editor::SavePin()
{
    const std::string path = dataDir_ + "pins.json";
    json              pins = ReadPins(path);
    if (pins.is_null())
    {
        TraceLog(LOG_WARNING, "Editor: %s is not valid JSON -- not saved", path.c_str());
        return;
    }
    // The server would refuse a pin that breaks the system (#237); writing one would only
    // move the refusal to the next start. The editor's own verbs keep these rules, so this
    // is the last line, not the first.
    std::vector<std::string> broken;
    Gen::CheckPinned(generated_.base, systemJson_, generated_.systems, generated_.id, broken);
    if (!broken.empty())
    {
        for (const std::string& b : broken)
            TraceLog(LOG_WARNING, "Editor: %s", b.c_str());
        Notice("Not saved: " + broken.front());
        return;
    }
    if (!pins.contains("note"))
        pins["note"] = "Hand-written exceptions to the generated region, applied after "
                       "generation (#147). See documents/world_format.md, pins.json.";
    const json pin = Gen::PinFor(generated_.id, generated_.seed, generated_.base, systemJson_,
                                 generated_.origins, generated_.everySeed);
    Gen::StorePin(pins, generated_.id, generated_.seed, pin, generated_.everySeed);

    std::ofstream out(path);
    if (!out.is_open())
    {
        TraceLog(LOG_WARNING, "Editor: failed to write %s", path.c_str());
        return;
    }
    out << pins.dump(4) << "\n";
    dirty_ = false;
    TraceLog(LOG_INFO, "Editor: %s %s of %s in %s",
             pin.is_null()
                 ? "no difference left; removed the pin for"
                 : (pin["mode"] == "merge" ? "pinned the difference to" : "pinned the whole of"),
             generated_.id.c_str(),
             generated_.everySeed ? "every seed"
                                  : TextFormat("seed %llu", (unsigned long long)generated_.seed),
             path.c_str());
    // A pin for every seed lands in a system that differs from seed to seed: by name and
    // position it finds what it means, by a place in a list it finds whatever is there.
    if (generated_.everySeed && !pin.is_null())
        Notice(Gen::ReplacesByIndex(pin)
                   ? "Saved for every seed -- but it names objects by their place in a list "
                     "(a planet), which is a different object in each seed's system"
                   : "Saved for every seed: placed by name and position in a system that "
                     "differs from seed to seed");
}

void Editor::ToggleEverySeed()
{
    if (!generated_.open)
        return;
    if (dirty_)
    {
        Notice("Save or undo the changes first: switching reopens the system");
        return;
    }
    const uint64_t    seed = generated_.seed;
    const std::string id = generated_.id;
    const bool        every = !generated_.everySeed;
    if (OpenGenerated(seed, id, every))
        Notice(every ? "Editing this system's pins for EVERY seed (Ctrl+E: back to this seed)"
                     : TextFormat("Editing this system's pins for seed %llu only",
                                  (unsigned long long)seed));
}

Editor::Provenance Editor::ProvenanceOf(const ObjHandle& h) const
{
    if (h.category == "star" || h.category == "stars")  // a pin sets them whole
        return systemJson_.value(h.category, json()) == generated_.base.value(h.category, json())
                   ? Provenance::Generated
                   : Provenance::Changed;
    const auto o = generated_.origins.find(h.category);
    if (o == generated_.origins.end() || h.index < 0 || h.index >= (int)o->second.size() ||
        o->second[(size_t)h.index] < 0)
        return Provenance::Added;
    const int from = o->second[(size_t)h.index];
    if (!generated_.base.contains(h.category) || from >= (int)generated_.base[h.category].size())
        return Provenance::Added;
    // A satellite only re-pointed because a planet before its own went is not changed:
    // the pin says nothing about it (#237).
    const size_t basePlanets =
        generated_.base.contains("planets") && generated_.base["planets"].is_array()
            ? generated_.base["planets"].size()
            : 0;
    const json alone = Gen::Repointed(generated_.base[h.category][(size_t)from],
                                      Gen::PlanetMap(basePlanets, generated_.origins));
    return systemJson_[h.category][(size_t)h.index] == alone ? Provenance::Generated
                                                             : Provenance::Changed;
}

void Editor::Notice(const std::string& text)
{
    TraceLog(LOG_INFO, "Editor: %s", text.c_str());
    notice_ = text;
    noticeUntil_ = GetTime() + 6.0;
}
