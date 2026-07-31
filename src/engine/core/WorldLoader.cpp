#include "core/WorldLoader.h"

#include "entities/Star.h"
#include "entities/Planet.h"
#include "entities/Station.h"
#include "entities/AsteroidField.h"
#include "entities/Nebula.h"
#include "entities/Derelict.h"
#include "entities/JumpGate.h"
#include "core/Faction.h"
#include "economy/Resource.h"
#include "raylib.h"
#include <nlohmann/json.hpp>
#include <fstream>

using json = nlohmann::json;

static StarType StarTypeFromString(const std::string& s)
{
    if (s == "Red")
        return StarType::Red;
    if (s == "Blue")
        return StarType::Blue;
    return StarType::Yellow;
}

static ResourceType ResourceFromString(const std::string& s)
{
    if (s == "Ice")
        return ResourceType::Ice;
    if (s == "Crystal")
        return ResourceType::Crystal;
    return ResourceType::Iron;
}

static PlanetType PlanetTypeFromString(const std::string& s)
{
    if (s == "Gas")
        return PlanetType::Gas;
    if (s == "Ice")
        return PlanetType::Ice;
    if (s == "Lava")
        return PlanetType::Lava;
    if (s == "Oceanic")
        return PlanetType::Oceanic;
    return PlanetType::Rocky;
}

static StationRole StationRoleFromString(const std::string& s)
{
    if (s == "MiningOutpost")
        return StationRole::MiningOutpost;
    if (s == "Shipyard")
        return StationRole::Shipyard;
    if (s == "Military")
        return StationRole::Military;
    return StationRole::TradeHub;
}

static Vector2 Vec2FromJson(const json& arr)
{
    return Vector2{ (float)arr[0], (float)arr[1] };
}

static Color ColorFromJson(const json& arr)
{
    return Color{ (unsigned char)arr[0], (unsigned char)arr[1], (unsigned char)arr[2], 255 };
}

WorldLoader::Universe WorldLoader::LoadUniverse(const std::string& path)
{
    Universe universe;

    std::ifstream file(path);
    if (!file.is_open())
    {
        TraceLog(LOG_WARNING, "WorldLoader: galaxy index not found %s", path.c_str());
        return universe;
    }

    json data = json::parse(file, nullptr, false);
    if (data.is_discarded())
    {
        TraceLog(LOG_WARNING, "WorldLoader: parse error %s", path.c_str());
        return universe;
    }

    universe.startId = data.value("start", std::string(""));
    for (const json& s : data["systems"])
    {
        SystemInfo info;
        info.id = s["id"];
        info.name = s.value("name", info.id);
        info.file = s["file"];
        info.mapPos = s.contains("map") ? Vec2FromJson(s["map"]) : Vector2{ 0.0f, 0.0f };
        info.security = (float)s.value("security", 0.5);
        info.owner = s.value("owner", std::string());
        universe.systems.push_back(info);
    }

    if (data.contains("links"))
        for (const json& l : data["links"])
            universe.links.push_back(SystemLink{ l[0], l[1] });

    if (universe.startId.empty() && !universe.systems.empty())
        universe.startId = universe.systems.front().id;

    TraceLog(LOG_INFO, "WorldLoader: systems in galaxy: %d", (int)universe.systems.size());
    return universe;
}

std::vector<std::unique_ptr<Entity>> WorldLoader::LoadSystem(const std::string& path)
{
    std::ifstream file(path);
    if (!file.is_open())
    {
        TraceLog(LOG_WARNING, "WorldLoader: file not found %s", path.c_str());
        return {};
    }

    // Third argument false — don't throw on a parse error.
    json data = json::parse(file, nullptr, false);
    if (data.is_discarded())
    {
        TraceLog(LOG_WARNING, "WorldLoader: JSON parse error in %s", path.c_str());
        return {};
    }

    return BuildSystem(data);
}

// Builds entities from already-parsed JSON. Order: star, planets, stations,
// asteroid fields, nebulae, derelicts, gates — the editor relies on it (entity
// indices correspond to JSON elements).
std::vector<std::unique_ptr<Entity>> WorldLoader::BuildSystem(const json& data)
{
    std::vector<std::unique_ptr<Entity>> entities;

    // Star (optional — new/minimal systems may not have one).
    if (data.contains("star"))
    {
        const json& starJson = data["star"];
        StarType    starType = StarTypeFromString(starJson.value("type", std::string("Yellow")));
        float       starSize = (float)starJson.value("size", 400.0);
        entities.push_back(std::make_unique<Star>(Vector2{ 0.0f, 0.0f }, starSize, starType));
    }

    if (data.contains("planets"))
        for (const json& p : data["planets"])
        {
            PlanetType type = PlanetTypeFromString(p.value("type", std::string("Rocky")));
            // Color is optional: if not set, the planet's default type color is used.
            Color color =
                p.contains("color") ? ColorFromJson(p["color"]) : PlanetTypeColor(type);
            entities.push_back(std::make_unique<Planet>(
                p.value("orbitRadius", 5000.0), p.value("orbitSpeed", 300.0),
                p.value("angle", 0.0), p.value("size", 100.0), color,
                ResourceFromString(p.value("deposit", std::string("Iron"))), type));
        }

    if (data.contains("stations"))
    {
        for (const json& s : data["stations"])
        {
            FactionId faction =
                FactionFromString(s.value("faction", std::string("Independent")));
            StationRole role =
                StationRoleFromString(s.value("role", std::string("TradeHub")));
            entities.push_back(std::make_unique<Station>(
                Vec2FromJson(s["pos"]), (float)s["size"], s["name"], faction, role));
        }
    }

    if (data.contains("asteroidFields"))
    {
        for (const json& f : data["asteroidFields"])
        {
            entities.push_back(std::make_unique<AsteroidField>(
                Vec2FromJson(f["pos"]), (float)f["size"], f["name"],
                ResourceFromString(f["resource"]), (int)f["ore"]));
        }
    }

    if (data.contains("nebulae"))
    {
        for (const json& n : data["nebulae"])
        {
            entities.push_back(std::make_unique<Nebula>(
                Vec2FromJson(n["pos"]), (float)n["radius"],
                n.value("name", std::string("Nebula"))));
        }
    }

    if (data.contains("derelicts"))
    {
        for (const json& d : data["derelicts"])
        {
            entities.push_back(std::make_unique<Derelict>(
                Vec2FromJson(d["pos"]), (float)d.value("size", 16.0), d["name"],
                (double)d.value("reward", 500.0)));
        }
    }

    if (data.contains("gates"))
    {
        for (const json& g : data["gates"])
        {
            entities.push_back(std::make_unique<JumpGate>(
                Vec2FromJson(g["pos"]), (float)g["size"], g["name"],
                g.value("destination", std::string("Unknown"))));
        }
    }

    return entities;
}
