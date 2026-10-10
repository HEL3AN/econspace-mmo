#include "core/WorldLoader.h"

#include "core/Orbits.h"
#include "core/Archetype.h"

#include "entities/Star.h"
#include "entities/Planet.h"
#include "entities/Station.h"
#include "entities/AsteroidField.h"
#include "entities/Nebula.h"
#include "entities/Derelict.h"
#include "entities/JumpGate.h"
#include "entities/Structure.h"
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

// An object may say which archetype of its kind it is (#142): a derelict that is a leviathan,
// a belt that is a motherlode (#211), later a station a player designed (#44). Without the
// field an object is its kind's ordinary archetype, as it always was. A name that is not an
// archetype -- or is one of another kind, a gate in the derelicts list -- is said by name and
// ignored, so one typo neither empties a system nor quietly builds the wrong thing.
static void ApplyArchetype(Entity& e, const json& o)
{
    if (!o.is_object() || !o.contains("archetype") || !o["archetype"].is_string())
        return;
    const std::string id = o["archetype"];
    const Archetype*  a = Archetypes::Find(id);
    if (a == nullptr || a->kind != e.GetKind())
    {
        TraceLog(LOG_WARNING, "WorldLoader: '%s' names archetype '%s', which %s -- built as usual",
                 o.value("name", std::string("an object")).c_str(), id.c_str(),
                 a == nullptr ? "does not exist" : "is a different kind of thing");
        return;
    }
    e.SetArchetype(id);
}

// A satellite (#210): `"orbits": { "planet": i, "radius": r, "speed": s, "phase": p }`.
// The position is then the planet's plus a turning offset, so "pos" may be left out; one
// that is given is where the object stands until the clock first places it.
static void ApplyOrbit(Entity& e, const json& o)
{
    if (!o.contains("orbits"))
        return;
    const json& j = o["orbits"];
    if (!j.is_object() || !j.contains("planet") || !j.contains("radius"))
    {
        TraceLog(LOG_WARNING,
                 "WorldLoader: '%s' has an orbits block without planet and radius "
                 "-- left where it stands",
                 o.value("name", std::string("an object")).c_str());
        return;
    }
    Orbit orbit;
    orbit.planet = j["planet"].get<int>();
    orbit.radius = j["radius"].get<float>();
    orbit.speed = j.value("speed", 0.0f);
    orbit.phase = j.value("phase", 0.0f);
    e.SetOrbit(orbit);
}

static json OrbitJson(const Orbit& orbit)
{
    return { { "planet", orbit.planet },
             { "radius", orbit.radius },
             { "speed", orbit.speed },
             { "phase", orbit.phase } };
}

static Vector2 PosOf(const json& o)
{
    return o.contains("pos") ? Vec2FromJson(o["pos"]) : Vector2{ 0.0f, 0.0f };
}

// Builds entities from already-parsed JSON. Order: star, planets, stations,
// asteroid fields, nebulae, derelicts, gates, structures — the editor relies on it (entity
// indices correspond to JSON elements).
std::vector<std::unique_ptr<Entity>> WorldLoader::BuildSystem(const json& data)
{
    std::vector<std::unique_ptr<Entity>> entities;

    // Star (optional — new/minimal systems may not have one).
    if (data.contains("star"))
    {
        const json& starJson = data["star"];
        StarType    starType = StarTypeFromString(starJson.value("type", std::string("Yellow")));
        float       starSize = (float)starJson.value("size", 150000.0);
        entities.push_back(std::make_unique<Star>(Vector2{ 0.0f, 0.0f }, starSize, starType));
        ApplyArchetype(*entities.back(), starJson);
    }
    // Or several, each where it says (#142): a binary lights a system from two sides
    // (#119), and nothing else about a star assumes it is alone.
    if (data.contains("stars") && data["stars"].is_array())
        for (const json& s : data["stars"])
        {
            const StarType type = StarTypeFromString(s.value("type", std::string("Yellow")));
            const Vector2  at = s.contains("pos") ? Vec2FromJson(s["pos"]) : Vector2{ 0.0f, 0.0f };
            entities.push_back(std::make_unique<Star>(at, (float)s.value("size", 150000.0), type));
            ApplyArchetype(*entities.back(), s);
        }

    if (data.contains("planets"))
        for (const json& p : data["planets"])
        {
            const std::string typeName = p.value("type", std::string("Rocky"));
            PlanetType        type = PlanetType::Rocky;
            if (!ParsePlanetType(typeName, type))
                TraceLog(LOG_WARNING, "WorldLoader: planet has unknown type '%s' -- built as Rocky",
                         typeName.c_str());
            // Color is optional: if not set, the planet's default type color is used.
            Color color = p.contains("color") ? ColorFromJson(p["color"]) : PlanetTypeColor(type);
            entities.push_back(std::make_unique<Planet>(
                p.value("orbitRadius", 350000.0), p.value("orbitSpeed", 30.0),
                p.value("angle", 0.0), p.value("size", 15000.0), color,
                ResourceFromString(p.value("deposit", std::string("Iron"))), type));
            ApplyArchetype(*entities.back(), p);
        }

    if (data.contains("stations"))
    {
        for (const json& s : data["stations"])
        {
            FactionId faction = FactionFromString(s.value("faction", std::string("Independent")));
            const std::string roleName = s.value("role", std::string("TradeHub"));
            StationRole       role = StationRole::TradeHub;
            // Still built, so one typo does not empty a system -- but said by name, since
            // the station it becomes is a different station (#191).
            if (!ParseStationRole(roleName, role))
                TraceLog(LOG_WARNING,
                         "WorldLoader: station '%s' has unknown role '%s' -- built as TradeHub",
                         s.value("name", std::string("?")).c_str(), roleName.c_str());
            entities.push_back(
                std::make_unique<Station>(PosOf(s), (float)s["size"], s["name"], faction, role));
            ApplyArchetype(*entities.back(), s);
            ApplyOrbit(*entities.back(), s);
        }
    }

    if (data.contains("asteroidFields"))
    {
        for (const json& f : data["asteroidFields"])
        {
            entities.push_back(
                std::make_unique<AsteroidField>(PosOf(f), (float)f["size"], f["name"],
                                                ResourceFromString(f["resource"]), (int)f["ore"]));
            ApplyArchetype(*entities.back(), f);
            ApplyOrbit(*entities.back(), f);
        }
    }

    if (data.contains("nebulae"))
    {
        for (const json& n : data["nebulae"])
        {
            entities.push_back(std::make_unique<Nebula>(PosOf(n), (float)n["radius"],
                                                        n.value("name", std::string("Nebula"))));
            ApplyArchetype(*entities.back(), n);
            ApplyOrbit(*entities.back(), n);
        }
    }

    if (data.contains("derelicts"))
    {
        for (const json& d : data["derelicts"])
        {
            entities.push_back(std::make_unique<Derelict>(PosOf(d), (float)d.value("size", 45.0),
                                                          d["name"],
                                                          (double)d.value("reward", 500.0)));
            ApplyArchetype(*entities.back(), d);
            ApplyOrbit(*entities.back(), d);
        }
    }

    if (data.contains("gates"))
    {
        for (const json& g : data["gates"])
        {
            entities.push_back(
                std::make_unique<JumpGate>(Vec2FromJson(g["pos"]), (float)g["size"], g["name"],
                                           g.value("destination", std::string("Unknown"))));
            ApplyArchetype(*entities.back(), g);
        }
    }

    // What players built (#39), last so nothing the data writes moves in the list. Unlike the
    // arrays above, an entry must say what it is: there is no ordinary structure for it to be.
    if (data.contains("structures"))
    {
        for (const json& t : data["structures"])
        {
            const std::string id = t.value("archetype", std::string());
            const Archetype*  a = Archetypes::Find(id);
            if (a == nullptr || a->kind != EntityKind::Structure)
            {
                TraceLog(LOG_WARNING,
                         "WorldLoader: structure '%s' names '%s', which is not a "
                         "structure archetype -- skipped",
                         t.value("name", std::string("?")).c_str(), id.c_str());
                continue;
            }
            auto st = std::make_unique<Structure>(PosOf(t), (float)t.value("size", 0.0),
                                                  t.value("name", a->name), id);
            st->StartBuilding(t.value("startedAt", 0.0), t.value("completesAt", 0.0));
            st->SetExpiresAt(t.value("expiresAt", 0.0));
            ApplyOrbit(*st, t);
            entities.push_back(std::move(st));
        }
    }

    Orbits::Place(entities, 0.0);  // satellites without a "pos" need one before anything asks
    return entities;
}

nlohmann::json WorldLoader::DescribeObject(const Entity& e, std::string& array)
{
    json o = json::object();
    array.clear();
    o["pos"] = { e.GetPosition().x, e.GetPosition().y };
    switch (e.GetKind())
    {
        case EntityKind::Station:
        {
            const Station& s = static_cast<const Station&>(e);
            array = "stations";
            o["name"] = s.GetName();
            o["size"] = s.GetSize();
            o["faction"] = Factions::Id(s.GetFaction());
            o["role"] = StationRoleId(s.GetRole());
            break;
        }
        case EntityKind::Field:
        {
            const AsteroidField& f = static_cast<const AsteroidField&>(e);
            array = "asteroidFields";
            o["name"] = f.GetName();
            o["size"] = f.GetSize();
            o["resource"] = ResourceName(f.GetResource());
            o["ore"] = f.GetOreMax();
            break;
        }
        case EntityKind::Nebula:
            array = "nebulae";
            o["name"] = e.GetName();
            o["radius"] = e.GetSize();
            break;
        case EntityKind::Derelict:
        {
            const Derelict& d = static_cast<const Derelict&>(e);
            array = "derelicts";
            o["name"] = d.GetBaseName();  // searched or not is state, not description
            o["size"] = d.GetSize();
            o["reward"] = d.GetReward();
            break;
        }
        case EntityKind::Structure:
        {
            // Its time line is description, not state: a site is described as one, and a
            // finished structure by what it is. Whose it is goes with the state.
            const Structure& t = static_cast<const Structure&>(e);
            array = "structures";
            o["name"] = t.GetName();
            o["size"] = t.GetSize();
            o["archetype"] = t.GetBuilds();
            if (t.IsBuilding())
            {
                o["startedAt"] = t.GetStartedAt();
                o["completesAt"] = t.GetCompletesAt();
            }
            if (t.GetExpiresAt() > 0.0)
                o["expiresAt"] = t.GetExpiresAt();
            if (t.GetOrbit().has_value())  // a faction's orbital outpost (#318)
                o["orbits"] = OrbitJson(*t.GetOrbit());
            return o;
        }
        case EntityKind::Star:
        case EntityKind::Planet:
        case EntityKind::Gate:
        case EntityKind::Npc:
        case EntityKind::PlayerShip:
        case EntityKind::Unknown: return json();
    }
    if (e.GetArchetype() != nullptr)
        o["archetype"] = e.GetArchetype()->id;
    if (e.GetOrbit().has_value())
        o["orbits"] = OrbitJson(*e.GetOrbit());
    return o;
}
