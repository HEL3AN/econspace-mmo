// Everything that changes universe.json: adding and removing systems, linking them,
// keeping the in-memory index in step with the file, and saving it.
//
// One translation unit of Editor (#17). A gate is a two-sided fact -- a link in
// universe.json and a gate object in each of the two system files -- and keeping both
// sides in one place is what stops them drifting apart.

#include "Editor.h"

#include "ui/UiTheme.h"
#include "ui/Button.h"
#include "raymath.h"
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using nlohmann::json;

#include "UniverseJson.h"
#include "core/World.h"

#include <fstream>

Vector2 MapPos(const nlohmann::json& sys)
{
    if (sys.contains("map") && sys["map"].is_array() && sys["map"].size() >= 2)
        return { (float)sys["map"][0].get<double>(), (float)sys["map"][1].get<double>() };
    return { 0.0f, 0.0f };
}

bool Editor::HasLink(const std::string& a, const std::string& b) const
{
    if (!universeJson_.contains("links"))
        return false;
    for (const auto& l : universeJson_["links"])
        if (l.is_array() && l.size() >= 2)
        {
            std::string la = l[0], lb = l[1];
            if ((la == a && lb == b) || (la == b && lb == a))
                return true;
        }
    return false;
}

void Editor::ToggleLink(const std::string& a, const std::string& b)
{
    if (!universeJson_.contains("links") || !universeJson_["links"].is_array())
        universeJson_["links"] = json::array();
    json& links = universeJson_["links"];
    for (int i = 0; i < (int)links.size(); i++)
    {
        if (!links[i].is_array() || links[i].size() < 2)
            continue;
        std::string la = links[i][0], lb = links[i][1];
        if ((la == a && lb == b) || (la == b && lb == a))
        {
            links.erase(i);  // link existed — remove it
            return;
        }
    }
    links.push_back(json::array({ a, b }));  // didn't exist — add it
}

void Editor::AddSystem()
{
    if (!universeJson_.contains("systems") || !universeJson_["systems"].is_array())
        universeJson_["systems"] = json::array();

    // Unique id.
    auto idExists = [&](const std::string& id)
    {
        for (const auto& s : universeJson_["systems"])
            if (s.value("id", std::string()) == id)
                return true;
        return false;
    };
    std::string id;
    for (int n = 1;; n++)
    {
        id = "sys" + std::to_string(n);
        if (!idExists(id))
            break;
    }

    json s = { { "id", id },
               { "name", "New System" },
               { "file", id + ".json" },
               { "map", { (int)roundf(camera_.target.x), (int)roundf(camera_.target.y) } } };
    universeJson_["systems"].push_back(s);

    // Create a minimal system file if it doesn't exist yet.
    std::string path = dataDir_ + "systems/" + id + ".json";
    if (!FileExists(path.c_str()))
    {
        std::ofstream o(path);
        if (o.is_open())
        {
            json def = { { "star", { { "type", "Yellow" }, { "size", 500 } } } };
            o << def.dump(4) << "\n";
        }
    }

    universeDirty_ = true;
    gSelected_ = (int)universeJson_["systems"].size() - 1;
}

void Editor::DeleteSystem(int index)
{
    if (!universeJson_.contains("systems") || index < 0 ||
        index >= (int)universeJson_["systems"].size())
        return;
    std::string id = universeJson_["systems"][index].value("id", std::string());

    // Partners of the system being deleted — remove the gates leading to it from them.
    std::vector<std::string> partners;
    if (universeJson_.contains("links"))
        for (const auto& l : universeJson_["links"])
            if (l.is_array() && l.size() >= 2)
            {
                std::string la = l[0], lb = l[1];
                if (la == id)
                    partners.push_back(lb);
                else if (lb == id)
                    partners.push_back(la);
            }
    for (const std::string& p : partners)
        SyncGate(p, id, false);  // remove the partner's gate to the deleted system

    // Remove links involving this system.
    if (universeJson_.contains("links") && universeJson_["links"].is_array())
    {
        json kept = json::array();
        for (const auto& l : universeJson_["links"])
            if (!(l.is_array() && l.size() >= 2 && (l[0] == id || l[1] == id)))
                kept.push_back(l);
        universeJson_["links"] = kept;
    }

    universeJson_["systems"].erase(index);
    gSelected_ = -1;
    gLinksOwner_.clear();
    universeDirty_ = true;
}

void Editor::RefreshUniverseStruct()
{
    universe_.systems.clear();
    universe_.links.clear();
    universe_.startId = universeJson_.value("start", std::string());

    if (universeJson_.contains("systems"))
        for (const auto& s : universeJson_["systems"])
        {
            WorldLoader::SystemInfo info;
            info.id = s.value("id", std::string());
            info.name = s.value("name", info.id);
            info.file = s.value("file", info.id + ".json");
            info.mapPos = MapPos(s);
            universe_.systems.push_back(info);
        }
    if (universeJson_.contains("links"))
        for (const auto& l : universeJson_["links"])
            if (l.is_array() && l.size() >= 2)
                universe_.links.push_back(WorldLoader::SystemLink{ l[0], l[1] });

    if (universe_.startId.empty() && !universe_.systems.empty())
        universe_.startId = universe_.systems.front().id;
}

void Editor::SaveUniverse()
{
    std::ofstream out(dataDir_ + "universe.json");
    if (!out.is_open())
    {
        TraceLog(LOG_WARNING, "Editor: failed to write universe.json");
        return;
    }
    out << universeJson_.dump(4) << "\n";
    universeDirty_ = false;
    RefreshUniverseStruct();
    TraceLog(LOG_INFO, "Editor: saved universe.json");
}

// Switch from galaxy mode into the selected system (by index in universeJson_).
void Editor::OpenSystem(int index)
{
    RefreshUniverseStruct();
    if (index < 0 || index >= (int)universe_.systems.size())
        return;
    currentSystem_ = index;
    EnterGalaxyMode(false);  // switches to system mode and loads currentSystem_
}

// Creates/removes in system sysId's file a gate leading to destId. The gate is
// positioned toward destId on the galaxy map, at the edge of the system. The file
// is read and rewritten in place (the "gate" change is applied immediately).
void Editor::SyncGate(const std::string& sysId, const std::string& destId, bool add)
{
    const json* sysEntry = nullptr;
    const json* destEntry = nullptr;
    if (universeJson_.contains("systems"))
        for (const auto& s : universeJson_["systems"])
        {
            if (s.value("id", std::string()) == sysId)
                sysEntry = &s;
            if (s.value("id", std::string()) == destId)
                destEntry = &s;
        }
    if (sysEntry == nullptr)
        return;

    std::string file = sysEntry->value("file", sysId + ".json");
    std::string path = dataDir_ + "systems/" + file;

    json data = json::object();
    {
        std::ifstream f(path);
        if (f.is_open())
        {
            data = json::parse(f, nullptr, false);
            if (data.is_discarded())
                data = json::object();
        }
    }
    if (!data.contains("gates") || !data["gates"].is_array())
        data["gates"] = json::array();

    if (add)
    {
        for (const auto& g : data["gates"])  // already there — do nothing
            if (g.value("destination", std::string()) == destId)
                return;

        Vector2 sp = MapPos(*sysEntry);
        Vector2 dp = (destEntry != nullptr) ? MapPos(*destEntry) : Vector2{ sp.x + 1.0f, sp.y };
        float   dx = dp.x - sp.x, dy = dp.y - sp.y;
        float   len = sqrtf(dx * dx + dy * dy);
        if (len < 0.001f)
        {
            dx = 1.0f;
            dy = 0.0f;
            len = 1.0f;
        }
        float       reach = World::SYSTEM_RADIUS * 0.85f;
        int         gx = (int)roundf(dx / len * reach);
        int         gy = (int)roundf(dy / len * reach);
        std::string destName = (destEntry != nullptr) ? destEntry->value("name", destId) : destId;
        data["gates"].push_back(json{ { "name", "Gate to " + destName },
                                      { "pos", { gx, gy } },
                                      { "size", 150 },
                                      { "destination", destId } });
    }
    else
    {
        json kept = json::array();
        for (const auto& g : data["gates"])
            if (g.value("destination", std::string()) != destId)
                kept.push_back(g);
        data["gates"] = kept;
    }

    std::ofstream o(path);
    if (o.is_open())
        o << data.dump(4) << "\n";
}
