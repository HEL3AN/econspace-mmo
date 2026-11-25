#include "core/Blueprint.h"

#include "core/Archetype.h"
#include "core/Fitting.h"
#include "core/JsonKeys.h"
#include <nlohmann/json.hpp>
#include <cmath>
#include <fstream>
#include <map>

using json = nlohmann::json;

namespace
{
using Blueprints::Current;

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
                         "wreck", "placement", "builders", "onto", "tier", "holds" },
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
    if (j.contains("onto"))
    {
        const json& onto = j["onto"];
        if (!onto.is_array() || onto.empty())
        {
            err = "'onto' is a list of the blueprints an upgrade is built onto";
            return false;
        }
        for (const json& o : onto)
        {
            if (!o.is_string() || o.get<std::string>().empty())
            {
                err = "onto: '" + o.dump() + "' is not a blueprint id";
                return false;
            }
            b.onto.push_back(o.get<std::string>());
        }
    }

    // Built from an archetype that exists, is a structure and says it can be built. Each
    // of the three is a different mistake, so each says which.
    const Archetype* a = Archetypes::Find(b.archetype);
    if (a == nullptr)
    {
        err = "archetype '" + b.archetype + "' does not exist";
        return false;
    }
    if (a->kind != EntityKind::Structure && (a->kind != EntityKind::Station || b.onto.empty()))
    {
        err = "archetype '" + b.archetype + "' is not a Structure" +
              (a->kind == EntityKind::Station ? " (a station is built only as an upgrade, 'onto')"
                                              : "");
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
        // ResourceFromName reads anything it does not know as the first good; here that would
        // be a misspelt Crystal quietly charged in iron.
        ResourceType r;
        if (!Goods::Find(it.key(), r))
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
        !ReadNumber(j, "lifetime", b.lifetime, err) || !ReadNumber(j, "hull", b.hull, err) ||
        !ReadNumber(j, "holds", b.holds, err))
        return false;
    if ((b.hull > 0.0f) == (b.holds > 0.0f))
    {
        err = "exactly one of 'hull' (points) and 'holds' (seconds the siege's reference fleet "
              "needs) is more than zero: anything buildable can be destroyed";
        return false;
    }
    b.tier = j.contains("tier") && j["tier"].is_string() ? j["tier"].get<std::string>() : "";
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
    if (!OnlyKnownKeys(p, { "reach", "clearance", "bodyClearance", "perSystem", "mouth" }, err))
    {
        err = "placement: " + err;
        return false;
    }
    float perSystem = 0.0f;
    if (!ReadNumber(p, "reach", b.reach, err) || !ReadNumber(p, "clearance", b.clearance, err) ||
        !ReadNumber(p, "bodyClearance", b.bodyClearance, err) ||
        !ReadNumber(p, "perSystem", perSystem, err) || !ReadNumber(p, "mouth", b.mouth, err))
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
namespace
{
Registry  g_process;  // what a process that never makes another registry reads and writes
Registry* g_current = &g_process;
}  // namespace

Registry& Current()
{
    return *g_current;
}

Scope::Scope(Registry& r) : previous_(g_current)
{
    g_current = &r;
}

Scope::~Scope()
{
    g_current = previous_;
}

bool Load(const std::string& path)
{
    Current().error.clear();
    std::ifstream in(path);
    if (!in.is_open())
    {
        Current().error = "cannot open " + path;
        return false;
    }
    const json j = json::parse(in, nullptr, false);
    if (j.is_discarded() || !j.is_object() || !j.contains("blueprints") ||
        !j["blueprints"].is_array())
    {
        Current().error = path + ": not a blueprint file (no 'blueprints' array)";
        return false;
    }
    std::string err;
    if (!OnlyKnownKeys(
            j, { "blueprints", "tiers", "limits", "dismantle", "destruction", "upkeep", "siege" },
            err))
    {
        Current().error = path + ": " + err;
        return false;
    }

    // Tiers (#431 D1): { name: { "mend": share of hull per second, "armour": share of hull that
    // is armour, "resist": { layer: { type: multiplier } } } }. Every blueprint names one; a
    // name the file does not define is an error, not a structure that never mends. Its walls
    // (#557) are read against the ship catalogue the way a hull line's are: no weapon family
    // may do less than `minEffect` to either layer, or the file does not load.
    struct Tier
    {
        std::string       name;
        float             mend = 0.0f, armour = 0.0f, mendShips = 0.0f;
        Ships::LayerTable resist = Ships::NeutralTable();
    };
    std::vector<Tier> tiers;
    const json        noTiers = json::object();
    const json&       tj = j.contains("tiers") ? j["tiers"] : noTiers;
    if (!tj.is_object())
    {
        Current().error = path + ": 'tiers' is an object of { tier: { \"mend\": share } }";
        return false;
    }
    const Ships::Catalogue& ships = Archetypes::ShipCatalogue();
    for (auto it = tj.begin(); it != tj.end(); ++it)
    {
        Tier t;
        t.name = it.key();
        if (!it.value().is_object() ||
            !OnlyKnownKeys(it.value(), { "mend", "mendShips", "armour", "resist" }, err) ||
            it.value().contains("mend") == it.value().contains("mendShips") ||
            !ReadShare(it.value(), "mend", t.mend, err) ||
            !ReadNumber(it.value(), "mendShips", t.mendShips, err) ||
            !ReadShare(it.value(), "armour", t.armour, err))
        {
            Current().error = path + ": tiers: '" + it.key() +
                              "': " + (err.empty() ? "is not { \"mend\": share }" : err);
            return false;
        }
        if (it.value().contains("resist") &&
            !Ships::ReadResist(it.value()["resist"], ships, t.resist, err))
        {
            Current().error = path + ": tiers: '" + it.key() + "': resist: " + err;
            return false;
        }
        if (!ships.damageTypes.empty())
            for (const Ships::Family& f : ships.families)
                for (int layer : { Ships::LayerArmour, Ships::LayerHull })
                {
                    const float e = Ships::Effect(ships, f.damage, t.resist, layer);
                    if (e < ships.minEffect - 1e-5f)
                    {
                        Current().error = path + ": tiers: '" + it.key() + "': family '" + f.id +
                                          "' does " + std::to_string(e) + " to its " +
                                          (layer == Ships::LayerArmour ? "armour" : "hull") +
                                          ", below minEffect " + std::to_string(ships.minEffect);
                        return false;
                    }
                }
        tiers.push_back(std::move(t));
    }

    std::vector<Blueprint> loaded;
    for (const json& bj : j["blueprints"])
    {
        Blueprint b;
        if (!ParseBlueprint(bj, b, err))
        {
            Current().error = path + ": blueprint '" + b.id + "': " + err;
            return false;
        }
        for (const Blueprint& other : loaded)
            if (other.id == b.id)
            {
                Current().error = path + ": blueprint '" + b.id + "' is defined twice";
                return false;
            }
        loaded.push_back(std::move(b));
    }

    // What an upgrade is built onto has to be a blueprint of the file, and not itself.
    for (const Blueprint& b : loaded)
        for (const std::string& o : b.onto)
        {
            bool found = false;
            for (const Blueprint& other : loaded)
                found = found || (other.id == o && other.id != b.id);
            if (!found)
            {
                Current().error = path + ": blueprint '" + b.id + "': onto: '" + o +
                                  "' is not another blueprint of the file";
                return false;
            }
        }

    // ...and every blueprint names a tier the file has.
    std::map<std::string, float> tierShips;  // a blueprint's tier's `mendShips`, 0: a share
    for (Blueprint& b : loaded)
    {
        bool tiered = false;
        for (const auto& t : tiers)
            if (t.name == b.tier)
            {
                b.mend = t.mend;
                tierShips[b.id] = t.mendShips;
                b.armour = t.armour;
                b.resist = t.resist;
                tiered = true;
            }
        if (!tiered)
        {
            Current().error =
                path + ": blueprint '" + b.id + "': " +
                (b.tier.empty() ? std::string("'tier' is missing")
                                : "tier '" + b.tier + "' is not one of the file's 'tiers'") +
                ": what it mends is set by its tier";
            return false;
        }
    }

    int perAccount = 0;
    if (j.contains("limits"))
    {
        const json& l = j["limits"];
        float       n = 0.0f;
        if (!OnlyKnownKeys(l, { "perAccount" }, err) || !ReadNumber(l, "perAccount", n, err))
        {
            Current().error = path + ": limits: " + err;
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
            Current().error = path + ": dismantle: " + (err.empty() ? "not an object" : err);
            return false;
        }
    }
    if (j.contains("destruction"))
    {
        const json& d = j["destruction"];
        if (!d.is_object() ||
            !OnlyKnownKeys(d, { "siteHull", "wreckShare", "wreckSeconds", "storageShare" }, err) ||
            !ReadShare(d, "siteHull", teardown.siteHull, err) ||
            !ReadShare(d, "wreckShare", teardown.wreckShare, err) ||
            !ReadShare(d, "storageShare", teardown.storageShare, err) ||
            !ReadNumber(d, "wreckSeconds", teardown.wreckSeconds, err))
        {
            Current().error = path + ": destruction: " + (err.empty() ? "not an object" : err);
            return false;
        }
        if (teardown.siteHull <= 0.0f)
        {
            Current().error = path +
                              ": destruction: 'siteHull' must be more than zero, or a new site "
                              "could not be hit at all";
            return false;
        }
    }

    Blueprints::Upkeep upkeep;
    if (j.contains("upkeep"))
    {
        const json& u = j["upkeep"];
        if (!u.is_object() || !OnlyKnownKeys(u, { "share", "decay" }, err) ||
            !ReadShare(u, "share", upkeep.share, err) || !ReadShare(u, "decay", upkeep.decay, err))
        {
            Current().error = path + ": upkeep: " + (err.empty() ? "not an object" : err);
            return false;
        }
    }
    Blueprints::Siege siege;
    float             referenceShips = 0.0f;
    if (j.contains("siege"))
    {
        const json& s = j["siege"];
        if (s.is_object() && s.contains("reference"))
        {
            if (!s["reference"].is_string())
            {
                Current().error = path + ": siege: 'reference' is a design id";
                return false;
            }
            siege.reference = s["reference"].get<std::string>();
        }
        // `dps` was a number standing in for the besiegers' guns; they are the faction's own
        // warship design now (Ships::WarshipDps). Named, so an old file says what to do.
        if (s.is_object() && s.contains("dps"))
        {
            Current().error =
                path + ": siege: 'dps' is gone -- a besieging ship hits as hard as its "
                       "faction's warship design (the doctrine's \"warship\" in ships.json); "
                       "remove the key";
            return false;
        }
        if (!s.is_object() ||
            !OnlyKnownKeys(s,
                           { "cost", "seconds", "jitter", "commit", "defence", "attrition", "shock",
                             "reach", "hop", "pool", "reference", "referenceShips" },
                           err) ||
            !ReadNumber(s, "cost", siege.cost, err) ||
            !ReadNumber(s, "seconds", siege.seconds, err) ||
            !ReadNumber(s, "jitter", siege.jitter, err) ||
            !ReadShare(s, "commit", siege.commit, err) ||
            !ReadNumber(s, "defence", siege.defence, err) ||
            !ReadShare(s, "attrition", siege.attrition, err) ||
            !ReadShare(s, "shock", siege.shock, err) || !ReadNumber(s, "reach", siege.reach, err) ||
            !ReadNumber(s, "hop", siege.hop, err) || !ReadNumber(s, "pool", siege.pool, err) ||
            !ReadNumber(s, "referenceShips", referenceShips, err))
        {
            Current().error = path + ": siege: " + (err.empty() ? "not an object" : err);
            return false;
        }
        siege.referenceShips = (int)referenceShips;
    }

    // The reference fleet (#568), and what is stated in its units derived into points.
    {
        const Ships::Catalogue& ships = Archetypes::ShipCatalogue();
        if (!siege.reference.empty())
        {
            const Ships::Fighter ref = Ships::DesignFighter(ships, siege.reference);
            if (ref.dps <= 0.0f || siege.referenceShips <= 0)
            {
                Current().error = path +
                                  ": siege: 'reference' names no armed design, or 'referenceShips' "
                                  "is not more than zero";
                return false;
            }
            siege.referenceShip = ref.dps;
            siege.referenceDps = ref.dps * (float)siege.referenceShips;
        }
        for (Blueprint& b : loaded)
        {
            const float ships_ = tierShips[b.id];
            if ((b.holds > 0.0f || ships_ > 0.0f) && siege.referenceDps <= 0.0f)
            {
                Current().error =
                    path + ": blueprint '" + b.id +
                    "': 'holds' and a tier's 'mendShips' are in the siege's reference "
                    "fleet's units: the 'siege' section needs 'reference' and "
                    "'referenceShips'";
                return false;
            }
            const float mendDps = ships_ * siege.referenceShip;
            if (b.holds > 0.0f)
            {
                if (siege.referenceDps <= mendDps)
                {
                    Current().error = path + ": blueprint '" + b.id + "': its tier mends " +
                                      std::to_string(mendDps) +
                                      " a second and the reference fleet "
                                      "fires " +
                                      std::to_string(siege.referenceDps) +
                                      ": no fleet of that strength could ever bring it down";
                    return false;
                }
                b.hull = b.holds * (siege.referenceDps - mendDps);
            }
            if (ships_ > 0.0f)
                b.mend = mendDps / b.hull;  // a share of its hull a second, as Structure reads it
        }
    }
    Current().forPlayers.clear();
    for (const Blueprint& b : loaded)
        if (b.byPlayers)
            Current().forPlayers.push_back(b);
    Current().all = std::move(loaded);
    Current().perAccount = perAccount;
    Current().teardown = teardown;
    Current().upkeep = upkeep;
    Current().siege = siege;
    return true;
}

float WallEffect(const Blueprint& bp, const Ships::TypeValues& mix)
{
    const Ships::Catalogue& ships = Archetypes::ShipCatalogue();
    if (ships.damageTypes.empty())
        return 1.0f;
    return Ships::PoolEffect(ships, mix, bp.resist, bp.armour * bp.hull,
                             (1.0f - bp.armour) * bp.hull);
}

const Blueprint* Find(const std::string& id)
{
    for (const Blueprint& b : Current().all)
        if (b.id == id)
            return &b;
    return nullptr;
}

const Blueprint* Building(const std::string& archetype)
{
    for (const Blueprint& b : Current().all)
        if (b.archetype == archetype)
            return &b;
    return nullptr;
}

const std::vector<Blueprint>& All()
{
    return Current().forPlayers;
}

int PerAccount()
{
    return Current().perAccount;
}

const Teardown& Rules()
{
    return Current().teardown;
}

const Upkeep& UpkeepRules()
{
    return Current().upkeep;
}

const Siege& SiegeRules()
{
    return Current().siege;
}

std::vector<std::pair<ResourceType, int>> Refund(const Blueprint& bp, float progress,
                                                 float                                hullFraction,
                                                 const std::vector<const Blueprint*>& earlier)
{
    const float p = progress < 0.0f ? 0.0f : (progress > 1.0f ? 1.0f : progress);
    const float h = hullFraction < 0.0f ? 0.0f : (hullFraction > 1.0f ? 1.0f : hullFraction);
    const float share = (1.0f - p * (1.0f - Current().teardown.refund)) * h;
    // Each good in the order it is first named, the tiers below first.
    std::vector<std::pair<ResourceType, double>> sum;
    auto                                         add = [&sum](const Blueprint& b, float s)
    {
        for (const auto& c : b.cost)
        {
            size_t i = 0;
            while (i < sum.size() && sum[i].first != c.first)
                i++;
            if (i == sum.size())
                sum.emplace_back(c.first, 0.0);
            sum[i].second += (double)c.second * (double)s;
        }
    };
    for (const Blueprint* b : earlier)
        if (b != nullptr)
            add(*b, Current().teardown.refund * h);  // finished, and as hurt as the whole of it
    add(bp, share);
    std::vector<std::pair<ResourceType, int>> back;
    for (const auto& c : sum)
    {
        // A hair under the product before rounding down: 10 x 0.3 is 2.9999998 in a float,
        // and the refund the rule states is 3, not 2.
        const int n = (int)std::floor(c.second + 1e-4);
        if (n > 0)
            back.emplace_back(c.first, n);
    }
    return back;
}

const std::string& Error()
{
    return Current().error;
}
}  // namespace Blueprints
