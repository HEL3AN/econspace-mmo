#include "core/Archetype.h"

#include "render/Modules.h"

#include "core/JsonKeys.h"
#include "entities/Planet.h"
#include "entities/Station.h"
#include <nlohmann/json.hpp>
#include <fstream>

using json = nlohmann::json;

namespace
{
std::vector<Archetype> g_archetypes;
std::string            g_error;

EntityKind KindFromString(const std::string& s)
{
    if (s == "Star")
        return EntityKind::Star;
    if (s == "Planet")
        return EntityKind::Planet;
    if (s == "Station")
        return EntityKind::Station;
    if (s == "Field")
        return EntityKind::Field;
    if (s == "Gate")
        return EntityKind::Gate;
    if (s == "Nebula")
        return EntityKind::Nebula;
    if (s == "Derelict")
        return EntityKind::Derelict;
    if (s == "Npc")
        return EntityKind::Npc;
    if (s == "PlayerShip")
        return EntityKind::PlayerShip;
    return EntityKind::Unknown;
}

Color ColorFromJson(const json& j, Color fallback)
{
    if (!j.is_array() || j.size() < 3)
        return fallback;
    auto ch = [&](size_t i, unsigned char def) -> unsigned char
    {
        if (i >= j.size() || !j[i].is_number())
            return def;
        int v = j[i].get<int>();
        return (unsigned char)(v < 0 ? 0 : (v > 255 ? 255 : v));
    };
    return Color{ ch(0, fallback.r), ch(1, fallback.g), ch(2, fallback.b), ch(3, 255) };
}

// Reads one archetype. Returns false with `err` set on anything that would leave the
// entry unusable: unlike a snapshot field, a bad archetype is content the author can
// fix, and failing loudly at load beats an invisible object at runtime.
bool ParseArchetype(const json& j, Archetype& a, std::string& err)
{
    if (!j.is_object())
    {
        err = "archetype entry is not an object";
        return false;
    }
    a.id = j.value("id", std::string());
    if (a.id.empty())
    {
        err = "archetype without an id";
        return false;
    }
    // A misspelled field would otherwise be read as absent and take its default, which
    // is the same silent wrong object an unknown component is (#191).
    if (!OnlyKnownKeys(j,
                       { "id", "name", "kind", "glyph", "sprite", "layer", "style", "material",
                         "shape", "light", "color", "size", "world", "components" },
                       err))
    {
        err = "archetype '" + a.id + "': " + err;
        return false;
    }
    a.name = j.value("name", a.id);
    a.kind = KindFromString(j.value("kind", std::string()));
    if (a.kind == EntityKind::Unknown)
    {
        err = "archetype '" + a.id + "' has an unknown kind";
        return false;
    }

    a.visual.glyph = j.value("glyph", std::string("?"));
    a.visual.sprite = j.value("sprite", std::string());
    a.visual.layer = j.value("layer", 0);

    const std::string style = j.value("style", std::string("point"));
    if (style == "point")
        a.visual.style = GlyphStyle::Point;
    else if (style == "region")
        a.visual.style = GlyphStyle::Region;
    else if (style == "directional")
        a.visual.style = GlyphStyle::Directional;
    else
    {
        err = "archetype '" + a.id + "': unknown style '" + style + "'";
        return false;
    }
    a.visual.material = j.value("material", std::string());

    if (j.contains("shape"))
    {
        std::string why;
        if (!Render::ParseShape(j["shape"], a.visual.shape, why))
        {
            err = "archetype '" + a.id + "': " + why;
            return false;
        }
    }

    if (j.contains("light") && j["light"].is_object())
    {
        if (!OnlyKnownKeys(j["light"], { "radius", "intensity" }, err))
        {
            err = "archetype '" + a.id + "': light: " + err;
            return false;
        }
        a.visual.lightRadius = j["light"].value("radius", 0.0f);
        a.visual.lightIntensity = j["light"].value("intensity", 1.0f);
    }

    if (j.contains("color"))
        a.visual.color = ColorFromJson(j["color"], a.visual.color);
    a.defaultSize = j.value("size", 0.0f);

    if (j.contains("world"))
    {
        const json& w = j["world"];
        if (!w.is_object())
        {
            err = "archetype '" + a.id + "': world is not an object";
            return false;
        }
        if (!OnlyKnownKeys(w, { "category", "subType" }, err))
        {
            err = "archetype '" + a.id + "': world: " + err;
            return false;
        }
        a.worldCategory = w.value("category", std::string());
        a.worldSubType = w.value("subType", std::string());
        if (a.worldCategory.empty())
        {
            err = "archetype '" + a.id + "': world block without a category";
            return false;
        }

        // The editor writes the subtype into a system file and the world loader reads it
        // back. Read back, a role it does not know is a trade hub and a planet type it
        // does not know is rocky -- so every object placed from this archetype would turn
        // into something else, with nothing anywhere to say so (#191).
        const std::string& c = a.worldCategory;
        const std::string& sub = a.worldSubType;
        if (c == "stations")
        {
            StationRole role;
            if (!ParseStationRole(sub, role))
            {
                err = "archetype '" + a.id + "': world.subType '" + sub +
                      "' is not a station role (TradeHub, MiningOutpost, Shipyard, Military)";
                return false;
            }
        }
        else if (c == "planets")
        {
            PlanetType type;
            if (!ParsePlanetType(sub, type))
            {
                err = "archetype '" + a.id + "': world.subType '" + sub +
                      "' is not a planet type (Rocky, Gas, Ice, Lava, Oceanic)";
                return false;
            }
        }
        else if (c == "asteroidFields" || c == "nebulae" || c == "derelicts" || c == "gates")
        {
            if (!sub.empty())
            {
                err = "archetype '" + a.id + "': category '" + c + "' has no subType";
                return false;
            }
        }
        else
        {
            err =
                "archetype '" + a.id + "': world.category '" + c + "' is not one a system file has";
            return false;
        }
    }

    if (!j.contains("components"))
        return true;
    const json& cs = j["components"];
    if (!cs.is_object())
    {
        err = "archetype '" + a.id + "': components is not an object";
        return false;
    }

    for (auto it = cs.begin(); it != cs.end(); ++it)
    {
        Component c = Component::Dockable;
        bool      known = false;
        for (Component candidate : AllComponents())
            if (it.key() == ComponentName(candidate))
            {
                c = candidate;
                known = true;
                break;
            }
        if (!known)
        {
            err = "archetype '" + a.id + "': unknown component '" + it.key() + "'";
            return false;
        }
        a.components.Add(c);

        const json& p = it.value();
        if (!p.is_object())
        {
            err = "archetype '" + a.id + "': component '" + it.key() + "' is not an object";
            return false;
        }

        // Every parameter a component reads, by name. A component with none takes an
        // empty list, so `"market": { "spread": 2 }` is refused rather than ignored.
        std::vector<const char*> params;
        switch (c)
        {
            case Component::Dockable:
            case Component::Salvageable:
            case Component::JumpLink: params = { "range" }; break;
            case Component::Mineable: params = { "extractRate", "range" }; break;
            case Component::Defensive: params = { "range", "damage" }; break;
            case Component::Storage: params = { "capacity" }; break;
            case Component::Hazard: params = { "radius", "hidesShips" }; break;
            case Component::Buildable: params = { "cost", "buildSeconds" }; break;
            case Component::Market: break;
        }
        if (!OnlyKnownKeys(p, params, err))
        {
            err = "archetype '" + a.id + "': component '" + it.key() + "': " + err;
            return false;
        }

        switch (c)
        {
            case Component::Dockable: a.dockRange = p.value("range", a.dockRange); break;
            case Component::Mineable:
                a.extractRate = p.value("extractRate", a.extractRate);
                a.extractRange = p.value("range", a.extractRange);
                break;
            case Component::Salvageable: a.salvageRange = p.value("range", a.salvageRange); break;
            case Component::JumpLink: a.jumpRange = p.value("range", a.jumpRange); break;
            case Component::Defensive:
                a.weaponRange = p.value("range", a.weaponRange);
                a.weaponDamage = p.value("damage", a.weaponDamage);
                break;
            // Parsed and checked so that data written now stays valid, but no pass reads
            // it: holding cargo off a ship belongs to the player-mutable world (#44).
            case Component::Storage:
                a.storageCapacity = p.value("capacity", a.storageCapacity);
                break;
            case Component::Hazard:
                a.hazardRadius = p.value("radius", a.hazardRadius);
                a.hazardHidesShips = p.value("hidesShips", a.hazardHidesShips);
                break;
            // Reserved for #44 like storage: nothing builds anything yet.
            case Component::Buildable:
                a.buildCost = p.value("cost", a.buildCost);
                a.buildSeconds = p.value("buildSeconds", a.buildSeconds);
                break;
            // No archetype-level parameters. What varies about a market is per system,
            // not per kind of station.
            case Component::Market: break;
        }
    }
    return true;
}
}  // namespace

const char* ComponentName(Component c)
{
    switch (c)
    {
        case Component::Dockable: return "dockable";
        case Component::Mineable: return "mineable";
        case Component::Market: return "market";
        case Component::Defensive: return "defensive";
        case Component::Storage: return "storage";
        case Component::JumpLink: return "jumpLink";
        case Component::Hazard: return "hazard";
        case Component::Salvageable: return "salvageable";
        case Component::Buildable: return "buildable";
    }
    return "unknown";
}

const std::vector<Component>& AllComponents()
{
    static const std::vector<Component> all = { Component::Dockable, Component::Mineable,
                                                Component::Market,   Component::Defensive,
                                                Component::Storage,  Component::JumpLink,
                                                Component::Hazard,   Component::Salvageable,
                                                Component::Buildable };
    return all;
}

namespace Archetypes
{
bool Load(const std::string& path)
{
    g_error.clear();

    // The module library first, from beside this file: archetypes name modules, and a
    // part naming one that is not loaded is a load error (#240).
    {
        const size_t      slash = path.find_last_of("/\\");
        const std::string dir =
            slash == std::string::npos ? std::string() : path.substr(0, slash + 1);
        std::string why;
        if (!Render::Modules::Load(dir + "modules.json", why))
        {
            g_error = why;
            return false;
        }
    }

    std::ifstream f(path);
    if (!f.is_open())
    {
        g_error = "cannot open " + path;
        return false;
    }

    json j;
    try
    {
        f >> j;
    }
    catch (const std::exception& e)
    {
        g_error = std::string("malformed JSON in ") + path + ": " + e.what();
        return false;
    }

    if (!j.contains("archetypes") || !j["archetypes"].is_array())
    {
        g_error = path + ": expected an \"archetypes\" array";
        return false;
    }

    std::vector<Archetype> loaded;
    for (const json& entry : j["archetypes"])
    {
        Archetype   a;
        std::string err;
        if (!ParseArchetype(entry, a, err))
        {
            g_error = path + ": " + err;
            return false;
        }
        for (const Archetype& seen : loaded)
            if (seen.id == a.id)
            {
                g_error = path + ": duplicate archetype id '" + a.id + "'";
                return false;
            }
        loaded.push_back(std::move(a));
    }

    // Swapped in only once the whole file parsed: a half-applied registry would be
    // worse than the previous one.
    g_archetypes = std::move(loaded);
    return true;
}

const Archetype* Find(const std::string& id)
{
    for (const Archetype& a : g_archetypes)
        if (a.id == id)
            return &a;
    return nullptr;
}

Archetype* Mutable(const std::string& id)
{
    for (Archetype& a : g_archetypes)
        if (a.id == id)
            return &a;
    return nullptr;
}

std::vector<const Archetype*> With(Component c)
{
    std::vector<const Archetype*> out;
    for (const Archetype& a : g_archetypes)
        if (a.Has(c))
            out.push_back(&a);
    return out;
}

std::vector<const Archetype*> OfKind(EntityKind kind)
{
    std::vector<const Archetype*> out;
    for (const Archetype& a : g_archetypes)
        if (a.kind == kind)
            out.push_back(&a);
    return out;
}

const std::vector<Archetype>& All()
{
    return g_archetypes;
}

const std::string& Error()
{
    return g_error;
}
}  // namespace Archetypes
