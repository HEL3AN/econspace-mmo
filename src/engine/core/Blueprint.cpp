#include "core/Blueprint.h"

#include "core/Archetype.h"
#include "core/JsonKeys.h"
#include <nlohmann/json.hpp>
#include <fstream>

using json = nlohmann::json;

namespace
{
std::vector<Blueprint> g_blueprints;
std::vector<Blueprint> g_forPlayers;  // the ones a player may build, as All() lists them
int                    g_perAccount = 0;
std::string            g_error;

// A non-negative number, or a load error naming the field: a cost of "ten" read as zero
// would be a free beacon.
bool ReadNumber(const json& j, const char* key, float& out, std::string& err)
{
    if (!j.contains(key))
        return true;
    if (!j[key].is_number() || j[key].get<double>() < 0.0)
    {
        err = std::string("'") + key + "' is not a non-negative number";
        return false;
    }
    out = j[key].get<float>();
    return true;
}

bool ParseBlueprint(const json& j, Blueprint& b, std::string& err)
{
    if (!j.is_object())
    {
        err = "blueprint entry is not an object";
        return false;
    }
    b.id = j.value("id", std::string());
    if (b.id.empty())
    {
        err = "blueprint without an id";
        return false;
    }
    if (!OnlyKnownKeys(j,
                       { "id", "name", "archetype", "cost", "buildSeconds", "lifetime", "placement",
                         "builders" },
                       err))
        return false;
    if (j.contains("builders"))
    {
        const json& who = j["builders"];
        if (!who.is_array() || who.empty())
        {
            err = "'builders' is a list of who may build it: \"player\", \"faction\"";
            return false;
        }
        b.byPlayers = b.byFactions = false;
        for (const json& w : who)
        {
            const std::string s = w.is_string() ? w.get<std::string>() : std::string();
            if (s == "player")
                b.byPlayers = true;
            else if (s == "faction")
                b.byFactions = true;
            else
            {
                err = "builders: '" + (w.is_string() ? s : w.dump()) +
                      "' is not \"player\" or \"faction\"";
                return false;
            }
        }
    }
    b.name = j.value("name", b.id);
    b.archetype = j.value("archetype", std::string());

    // Built from an archetype that exists, is a structure and says it can be built. Each
    // of the three is a different mistake, so each says which.
    const Archetype* a = Archetypes::Find(b.archetype);
    if (a == nullptr)
    {
        err = "archetype '" + b.archetype + "' does not exist";
        return false;
    }
    if (a->kind != EntityKind::Structure)
    {
        err = "archetype '" + b.archetype + "' is not a Structure";
        return false;
    }
    if (!a->Has(Component::Buildable))
    {
        err = "archetype '" + b.archetype + "' does not declare 'buildable'";
        return false;
    }

    if (!j.contains("cost") || !j["cost"].is_object() || j["cost"].empty())
    {
        err = "a blueprint costs something: 'cost' is an object of resource amounts";
        return false;
    }
    for (auto it = j["cost"].begin(); it != j["cost"].end(); ++it)
    {
        // ResourceFromName reads anything it does not know as Iron; here that would be a
        // misspelt Crystal quietly charged in iron.
        const ResourceType r = ResourceFromName(it.key());
        if (ResourceName(r) != it.key())
        {
            err = "cost: '" + it.key() + "' is not a resource";
            return false;
        }
        if (!it.value().is_number_integer() || it.value().get<int>() <= 0)
        {
            err = "cost: '" + it.key() + "' is not a positive whole amount";
            return false;
        }
        b.cost.emplace_back(r, it.value().get<int>());
    }

    if (!ReadNumber(j, "buildSeconds", b.buildSeconds, err) ||
        !ReadNumber(j, "lifetime", b.lifetime, err))
        return false;

    if (!j.contains("placement") || !j["placement"].is_object())
    {
        err = "'placement' is missing: where a thing may stand is not optional";
        return false;
    }
    const json& p = j["placement"];
    if (!OnlyKnownKeys(p, { "reach", "clearance", "bodyClearance", "perSystem" }, err))
    {
        err = "placement: " + err;
        return false;
    }
    float perSystem = 0.0f;
    if (!ReadNumber(p, "reach", b.reach, err) || !ReadNumber(p, "clearance", b.clearance, err) ||
        !ReadNumber(p, "bodyClearance", b.bodyClearance, err) ||
        !ReadNumber(p, "perSystem", perSystem, err))
    {
        err = "placement: " + err;
        return false;
    }
    b.perSystem = (int)perSystem;
    if (b.reach <= 0.0f)
    {
        err = "placement: 'reach' must be more than zero, or nothing could be placed";
        return false;
    }
    return true;
}
}  // namespace

namespace Blueprints
{
bool Load(const std::string& path)
{
    g_error.clear();
    std::ifstream in(path);
    if (!in.is_open())
    {
        g_error = "cannot open " + path;
        return false;
    }
    const json j = json::parse(in, nullptr, false);
    if (j.is_discarded() || !j.is_object() || !j.contains("blueprints") ||
        !j["blueprints"].is_array())
    {
        g_error = path + ": not a blueprint file (no 'blueprints' array)";
        return false;
    }
    std::string err;
    if (!OnlyKnownKeys(j, { "blueprints", "limits" }, err))
    {
        g_error = path + ": " + err;
        return false;
    }

    std::vector<Blueprint> loaded;
    for (const json& bj : j["blueprints"])
    {
        Blueprint b;
        if (!ParseBlueprint(bj, b, err))
        {
            g_error = path + ": blueprint '" + b.id + "': " + err;
            return false;
        }
        for (const Blueprint& other : loaded)
            if (other.id == b.id)
            {
                g_error = path + ": blueprint '" + b.id + "' is defined twice";
                return false;
            }
        loaded.push_back(std::move(b));
    }

    int perAccount = 0;
    if (j.contains("limits"))
    {
        const json& l = j["limits"];
        float       n = 0.0f;
        if (!OnlyKnownKeys(l, { "perAccount" }, err) || !ReadNumber(l, "perAccount", n, err))
        {
            g_error = path + ": limits: " + err;
            return false;
        }
        perAccount = (int)n;
    }

    g_forPlayers.clear();
    for (const Blueprint& b : loaded)
        if (b.byPlayers)
            g_forPlayers.push_back(b);
    g_blueprints = std::move(loaded);
    g_perAccount = perAccount;
    return true;
}

const Blueprint* Find(const std::string& id)
{
    for (const Blueprint& b : g_blueprints)
        if (b.id == id)
            return &b;
    return nullptr;
}

const std::vector<Blueprint>& All()
{
    return g_forPlayers;
}

int PerAccount()
{
    return g_perAccount;
}

const std::string& Error()
{
    return g_error;
}
}  // namespace Blueprints
