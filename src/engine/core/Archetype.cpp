#include "core/Archetype.h"

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
        a.worldCategory = w.value("category", std::string());
        a.worldSubType = w.value("subType", std::string());
        if (a.worldCategory.empty())
        {
            err = "archetype '" + a.id + "': world block without a category";
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
            case Component::Storage:
                a.storageCapacity = p.value("capacity", a.storageCapacity);
                break;
            case Component::Hazard:
                a.hazardRadius = p.value("radius", a.hazardRadius);
                a.hazardHidesShips = p.value("hidesShips", a.hazardHidesShips);
                break;
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
