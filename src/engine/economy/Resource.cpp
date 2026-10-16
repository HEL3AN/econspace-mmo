#include "economy/Resource.h"

#include "core/JsonKeys.h"

#include <nlohmann/json.hpp>
#include <fstream>
#include <map>
#include <utility>

using json = nlohmann::json;

namespace
{
// Everything goods.json says, as one value: a load builds a new one and swaps it in only when
// the whole file has read, so a file with a mistake leaves the registry as it was.
struct Registry
{
    std::vector<GoodDef>            goods;
    MarketRules                     rules;
    std::map<std::string, Industry> industries;  // by archetype id
    std::map<std::string, std::vector<std::pair<ResourceType, double>>> planets;
};

// The three ores of the original game, at the prices they always had, and no industry.
Registry Builtin()
{
    Registry r;
    auto     add = [&](const char* id, double price)
    {
        GoodDef d;
        d.id = d.name = id;
        d.price = price;
        d.mined = true;
        r.goods.push_back(d);
    };
    add("Iron", 10.0);
    add("Ice", 15.0);
    add("Crystal", 40.0);
    return r;
}

Registry    g_registry = Builtin();
std::string g_error;

int IndexOf(const Registry& r, const std::string& idOrName)
{
    for (size_t i = 0; i < r.goods.size(); i++)
        if (r.goods[i].id == idOrName)
            return (int)i;
    for (size_t i = 0; i < r.goods.size(); i++)
        if (r.goods[i].name == idOrName)
            return (int)i;
    return -1;
}

bool Number(const json& j, const char* key, double& out, std::string& why)
{
    if (!j.contains(key))
        return true;
    if (!j[key].is_number() || j[key].get<double>() < 0.0)
    {
        why = std::string("'") + key + "' is not a number of 0 or more";
        return false;
    }
    out = j[key].get<double>();
    return true;
}

// {"Iron": 3, "Ice": 1} -> amounts by good, each a positive number of a good in `r`.
bool Amounts(const Registry& r, const json& j, std::vector<std::pair<ResourceType, double>>& out,
             std::string& why)
{
    if (!j.is_object())
    {
        why = "an amount list is an object of good: amount";
        return false;
    }
    for (auto it = j.begin(); it != j.end(); ++it)
    {
        const int g = IndexOf(r, it.key());
        if (g < 0)
        {
            why = "no good called '" + it.key() + "'";
            return false;
        }
        if (!it.value().is_number() || it.value().get<double>() <= 0.0)
        {
            why = "the amount of '" + it.key() + "' is not a positive number";
            return false;
        }
        out.push_back({ ResourceType::FromIndex(g), it.value().get<double>() });
    }
    return true;
}

bool ParseGood(const json& j, GoodDef& d, std::string& why)
{
    if (!j.is_object() ||
        !OnlyKnownKeys(j, { "id", "name", "tier", "price", "stock", "supply", "mined" }, why))
    {
        if (why.empty())
            why = "a good is an object";
        return false;
    }
    if (!j.contains("id") || !j["id"].is_string() || j["id"].get<std::string>().empty())
    {
        why = "a good has no id";
        return false;
    }
    d.id = j["id"].get<std::string>();
    d.name = j.value("name", d.id);
    d.tier = j.value("tier", 0);
    d.mined = j.value("mined", false);
    const std::string supply = j.value("supply", std::string("local"));
    if (supply != "local" && supply != "imported")
    {
        why = "good '" + d.id + "': supply is 'local' or 'imported', not '" + supply + "'";
        return false;
    }
    d.supply = supply == "local" ? Supply::Local : Supply::Imported;
    if (!Number(j, "price", d.price, why) || !Number(j, "stock", d.stock, why) || d.price <= 0.0 ||
        d.stock < 1.0)
    {
        why = "good '" + d.id + "': " + (why.empty() ? "price and stock must be positive" : why);
        return false;
    }
    return true;
}

bool ParseIndustry(const Registry& r, const json& j, Industry& ind, std::string& why)
{
    if (!j.is_object() || !OnlyKnownKeys(j, { "recipes", "uses" }, why))
        return false;
    if (j.contains("uses") && !Amounts(r, j["uses"], ind.uses, why))
        return false;
    if (!j.contains("recipes"))
        return true;
    if (!j["recipes"].is_array())
    {
        why = "recipes is a list";
        return false;
    }
    for (const json& rj : j["recipes"])
    {
        Recipe rec;
        if (!rj.is_object() || !OnlyKnownKeys(rj, { "in", "out", "rate" }, why) ||
            !Amounts(r, rj.value("in", json::object()), rec.in, why) ||
            !Amounts(r, rj.value("out", json::object()), rec.out, why) ||
            !Number(rj, "rate", rec.rate, why))
        {
            if (why.empty())
                why = "a recipe is an object of in, out and rate";
            return false;
        }
        if (rec.out.empty() || rec.rate <= 0.0)
        {
            why = "a recipe makes something, at a positive rate";
            return false;
        }
        ind.recipes.push_back(rec);
    }
    return true;
}

bool Parse(const json& data, Registry& r, std::string& why)
{
    if (!data.is_object() ||
        !OnlyKnownKeys(data, { "market", "goods", "industries", "planets" }, why))
    {
        if (why.empty())
            why = "not an object";
        return false;
    }
    if (!data.contains("goods") || !data["goods"].is_array() || data["goods"].empty())
    {
        why = "no goods";
        return false;
    }
    for (const json& gj : data["goods"])
    {
        GoodDef d;
        if (!ParseGood(gj, d, why))
            return false;
        if (IndexOf(r, d.id) >= 0)
        {
            why = "good '" + d.id + "' is named twice";
            return false;
        }
        r.goods.push_back(d);
    }
    if (data.contains("market"))
    {
        const json&  m = data["market"];
        MarketRules& k = r.rules;
        if (!m.is_object() ||
            !OnlyKnownKeys(m,
                           { "fee", "spread", "elasticity", "floor", "ceiling", "beltMined",
                             "beltRegrow", "haulEvery", "haulCargo", "haulMargin" },
                           why) ||
            !Number(m, "fee", k.fee, why) || !Number(m, "spread", k.spread, why) ||
            !Number(m, "elasticity", k.elasticity, why) || !Number(m, "floor", k.floor, why) ||
            !Number(m, "ceiling", k.ceiling, why) || !Number(m, "beltMined", k.beltMined, why) ||
            !Number(m, "beltRegrow", k.beltRegrow, why) ||
            !Number(m, "haulEvery", k.haulEvery, why) ||
            !Number(m, "haulCargo", k.haulCargo, why) ||
            !Number(m, "haulMargin", k.haulMargin, why))
        {
            why = "market: " + (why.empty() ? std::string("not an object") : why);
            return false;
        }
        if (k.fee >= 1.0 || k.spread >= 1.0 || k.floor <= 0.0 || k.ceiling < k.floor)
        {
            why = "market: fee and spread are below 1, and 0 < floor <= ceiling";
            return false;
        }
    }
    if (data.contains("industries"))
    {
        if (!data["industries"].is_object())
        {
            why = "industries is an object of archetype: industry";
            return false;
        }
        for (auto it = data["industries"].begin(); it != data["industries"].end(); ++it)
        {
            Industry ind;
            if (!ParseIndustry(r, it.value(), ind, why))
            {
                why = "industry '" + it.key() + "': " + why;
                return false;
            }
            r.industries[it.key()] = ind;
        }
    }
    if (data.contains("planets"))
    {
        if (!data["planets"].is_object())
        {
            why = "planets is an object of archetype: yield";
            return false;
        }
        for (auto it = data["planets"].begin(); it != data["planets"].end(); ++it)
            if (!Amounts(r, it.value(), r.planets[it.key()], why))
            {
                why = "planets '" + it.key() + "': " + why;
                return false;
            }
    }
    return true;
}
}  // namespace

std::string ResourceName(ResourceType type)
{
    return Goods::Def(type).name;
}

ResourceType ResourceFromName(const std::string& name)
{
    ResourceType g;
    Goods::Find(name, g);
    return g;
}

std::vector<ResourceType> AllResourceTypes()
{
    std::vector<ResourceType> all;
    for (int i = 0; i < Goods::Count(); i++)
        all.push_back(ResourceType::FromIndex(i));
    return all;
}

namespace Goods
{
bool Load(const std::string& path)
{
    std::ifstream f(path);
    if (!f.is_open())
    {
        g_error = path + ": cannot open";
        return false;
    }
    json data = json::parse(f, nullptr, false);
    if (data.is_discarded())
    {
        g_error = path + ": not valid JSON";
        return false;
    }
    Registry    r;
    std::string why;
    if (!Parse(data, r, why))
    {
        g_error = path + ": " + why;
        return false;
    }
    g_registry = std::move(r);
    g_error.clear();
    return true;
}

const std::string& Error()
{
    return g_error;
}

void ResetToBuiltin()
{
    g_registry = Builtin();
}

int Count()
{
    return (int)g_registry.goods.size();
}

bool Find(const std::string& idOrName, ResourceType& out)
{
    const int i = IndexOf(g_registry, idOrName);
    if (i < 0)
        return false;
    out = ResourceType::FromIndex(i);
    return true;
}

const GoodDef& Def(ResourceType g)
{
    static const GoodDef none;
    return g.Index() < Count() ? g_registry.goods[(size_t)g.Index()] : none;
}

const std::string& Id(ResourceType g)
{
    return Def(g).id;
}

std::vector<ResourceType> Mined()
{
    std::vector<ResourceType> out;
    for (ResourceType g : AllResourceTypes())
        if (Def(g).mined)
            out.push_back(g);
    return out;
}

const MarketRules& Rules()
{
    return g_registry.rules;
}

const Industry* IndustryOf(const std::string& archetypeId)
{
    const auto it = g_registry.industries.find(archetypeId);
    return it == g_registry.industries.end() ? nullptr : &it->second;
}

const std::vector<std::pair<ResourceType, double>>* PlanetYield(const std::string& archetypeId)
{
    const auto it = g_registry.planets.find(archetypeId);
    return it == g_registry.planets.end() ? nullptr : &it->second;
}
}  // namespace Goods
