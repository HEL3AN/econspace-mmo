#include "core/Blueprint.h"

#include "core/Archetype.h"
#include "core/JsonKeys.h"
#include <nlohmann/json.hpp>
#include <cmath>
#include <fstream>

using json = nlohmann::json;

namespace
{
std::vector<Blueprint> g_blueprints;
std::vector<Blueprint> g_forPlayers;  // the ones a player may build, as All() lists them
int                    g_perAccount = 0;
Blueprints::Teardown   g_teardown;
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

// A share of something, 0..1, or a load error naming the field.
bool ReadShare(const json& j, const char* key, float& out, std::string& err)
{
    if (!ReadNumber(j, key, out, err))
        return false;
    if (out > 1.0f)
    {
        err = std::string("'") + key + "' is a share, at most 1";
        return false;
    }
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
                       { "id", "name", "archetype", "cost", "buildSeconds", "lifetime", "hull",
                         "wreck", "placement", "builders" },
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
        !ReadNumber(j, "lifetime", b.lifetime, err) || !ReadNumber(j, "hull", b.hull, err))
        return false;
    if (b.hull <= 0.0f)
    {
        err = "'hull' must be more than zero: anything buildable can be destroyed";
        return false;
    }
    if (j.contains("wreck"))
    {
        b.wreck = j["wreck"].is_string() ? j["wreck"].get<std::string>() : std::string();
        const Archetype* w = Archetypes::Find(b.wreck);
        if (w == nullptr || w->kind != EntityKind::Derelict)
        {
            err = "wreck: '" + (j["wreck"].is_string() ? b.wreck : j["wreck"].dump()) +
                  "' is not a Derelict archetype";
            return false;
        }
    }

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
    if (!OnlyKnownKeys(j, { "blueprints", "limits", "dismantle", "destruction" }, err))
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

    // Absent sections keep the defaults Teardown states; a present one is read as strictly
    // as a blueprint.
    Blueprints::Teardown teardown;
    if (j.contains("dismantle"))
    {
        const json& d = j["dismantle"];
        if (!d.is_object() || !OnlyKnownKeys(d, { "refund" }, err) ||
            !ReadShare(d, "refund", teardown.refund, err))
        {
            g_error = path + ": dismantle: " + (err.empty() ? "not an object" : err);
            return false;
        }
    }
    if (j.contains("destruction"))
    {
        const json& d = j["destruction"];
        if (!d.is_object() || !OnlyKnownKeys(d, { "siteHull", "wreckShare" }, err) ||
            !ReadShare(d, "siteHull", teardown.siteHull, err) ||
            !ReadShare(d, "wreckShare", teardown.wreckShare, err))
        {
            g_error = path + ": destruction: " + (err.empty() ? "not an object" : err);
            return false;
        }
        if (teardown.siteHull <= 0.0f)
        {
            g_error = path + ": destruction: 'siteHull' must be more than zero, or a new site "
                             "could not be hit at all";
            return false;
        }
    }

    g_forPlayers.clear();
    for (const Blueprint& b : loaded)
        if (b.byPlayers)
            g_forPlayers.push_back(b);
    g_blueprints = std::move(loaded);
    g_perAccount = perAccount;
    g_teardown = teardown;
    return true;
}

const Blueprint* Find(const std::string& id)
{
    for (const Blueprint& b : g_blueprints)
        if (b.id == id)
            return &b;
    return nullptr;
}

const Blueprint* Building(const std::string& archetype)
{
    for (const Blueprint& b : g_blueprints)
        if (b.archetype == archetype)
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

const Teardown& Rules()
{
    return g_teardown;
}

std::vector<std::pair<ResourceType, int>> Refund(const Blueprint& bp, float progress,
                                                 float hullFraction)
{
    const float p = progress < 0.0f ? 0.0f : (progress > 1.0f ? 1.0f : progress);
    const float h = hullFraction < 0.0f ? 0.0f : (hullFraction > 1.0f ? 1.0f : hullFraction);
    const float share = (1.0f - p * (1.0f - g_teardown.refund)) * h;
    std::vector<std::pair<ResourceType, int>> back;
    for (const auto& c : bp.cost)
    {
        // A hair under the product before rounding down: 10 x 0.3 is 2.9999998 in a float,
        // and the refund the rule states is 3, not 2.
        const int n = (int)std::floor((double)c.second * (double)share + 1e-4);
        if (n > 0)
            back.emplace_back(c.first, n);
    }
    return back;
}

const std::string& Error()
{
    return g_error;
}
}  // namespace Blueprints
