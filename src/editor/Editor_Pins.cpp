// Editing a generated system (#237). The generator is the default and a pin is the
// exception (#147), so the editor opens the system the server would build -- generated,
// then pinned -- lets the ordinary tools work on it, and saves only what was changed, as
// a pin. One translation unit of Editor (#17); the difference itself is Gen::PinFor, in
// the engine, where a test holds it to making the same system again.

#include "Editor.h"

#include <fstream>
#include <string>

using nlohmann::json;

Gen::RegionParams Editor::RegionParamsFromData(json& homeDoc) const
{
    Gen::RegionParams params;
    for (const WorldLoader::SystemInfo& info : universe_.systems)
    {
        params.knownMap.push_back(info.mapPos);
        if (info.id != universe_.startId)
            continue;
        params.homeId = info.id;
        params.homeMap = info.mapPos;
        std::ifstream in(dataDir_ + "systems/" + info.file);
        if (in.is_open())
            homeDoc = json::parse(in, nullptr, false);
    }
    if (params.homeId.empty())
        params.homeId = "home";
    if (homeDoc.is_object())
        params.homeSystem = &homeDoc;
    return params;
}

namespace
{
json ReadPins(const std::string& path)
{
    std::ifstream in(path);
    if (!in.is_open())
        return json::object();
    json pins = json::parse(in, nullptr, false);
    return pins.is_discarded() ? json() : pins;
}
}  // namespace

bool Editor::OpenGenerated(uint64_t seed, const std::string& systemId)
{
    json              homeDoc;
    Gen::RegionParams params = RegionParamsFromData(homeDoc);
    params.seed = seed;
    const Gen::Region region = Gen::GenerateRegion(params);
    const std::string id = systemId.empty() ? region.entryId : systemId;
    if (!region.documents.count(id))
    {
        std::string ids;
        for (const json& s : region.systems)
            ids += " " + s.value("id", std::string());
        TraceLog(LOG_WARNING, "Editor: seed %llu makes no system \"%s\"; it has:%s",
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
    Gen::OpenForEditing(region, pins, seed, id, es);
    for (const std::string& p : es.problems)
        TraceLog(LOG_WARNING, "Pins: %s -- not applied", p.c_str());

    generated_ = GeneratedEdit();
    generated_.open = true;
    generated_.seed = seed;
    generated_.id = id;
    generated_.name = id;
    for (const json& s : region.systems)
        if (s.value("id", std::string()) == id)
            generated_.name = s.value("name", id);
    generated_.base = es.base;
    generated_.origins = es.origins;
    generated_.problems = (int)es.problems.size();

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
    TraceLog(LOG_INFO, "Editor: opened generated system %s (%s) of seed %llu", id.c_str(),
             generated_.name.c_str(), (unsigned long long)seed);
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
    if (!pins.contains("note"))
        pins["note"] = "Hand-written exceptions to the generated region, applied after "
                       "generation (#147). See documents/world_format.md, pins.json.";
    const json pin = Gen::PinFor(generated_.id, generated_.seed, generated_.base, systemJson_,
                                 generated_.origins);
    Gen::StorePin(pins, generated_.id, generated_.seed, pin);

    std::ofstream out(path);
    if (!out.is_open())
    {
        TraceLog(LOG_WARNING, "Editor: failed to write %s", path.c_str());
        return;
    }
    out << pins.dump(4) << "\n";
    dirty_ = false;
    TraceLog(LOG_INFO, "Editor: %s %s of seed %llu in %s",
             pin.is_null()
                 ? "no difference left; removed the pin for"
                 : (pin["mode"] == "merge" ? "pinned the difference to" : "pinned the whole of"),
             generated_.id.c_str(), (unsigned long long)generated_.seed, path.c_str());
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
    return systemJson_[h.category][(size_t)h.index] == generated_.base[h.category][(size_t)from]
               ? Provenance::Generated
               : Provenance::Changed;
}
