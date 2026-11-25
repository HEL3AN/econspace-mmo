#include "economy/Resource.h"

#include "core/JsonKeys.h"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
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
    auto     add = [&](const char* id, double price, int volume)
    {
        GoodDef d;
        d.id = d.name = id;
        d.price = price;
        d.mined = true;
        d.volume = volume;
        r.goods.push_back(d);
    };
    add("Iron", 10.0, 10);
    add("Ice", 15.0, 10);
    add("Crystal", 40.0, 5);
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
    if (!j.is_object() || !OnlyKnownKeys(j,
                                         { "id", "name", "tier", "price", "stock", "supply",
                                           "mined", "volume", "module", "parts" },
                                         why))
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
    if (j.contains("volume") && (!j["volume"].is_number_integer() || j["volume"].get<int>() < 1 ||
                                 j["volume"].get<int>() > 1000))
    {
        why = "good '" + d.id + "': volume is a whole number from 1 to 1000";
        return false;
    }
    d.volume = j.value("volume", 1);
    const std::string supply = j.value("supply", std::string("local"));
    if (supply != "local" && supply != "imported")
    {
        why = "good '" + d.id + "': supply is 'local' or 'imported', not '" + supply + "'";
        return false;
    }
    d.supply = supply == "local" ? Supply::Local : Supply::Imported;
    if (j.contains("module"))
    {
        if (!j["module"].is_string() || j["module"].get<std::string>().empty() ||
            !j.contains("parts") || j.contains("price"))
        {
            why = "good '" + d.id +
                  "': a module names its part in 'module', its 'parts' and no price";
            return false;
        }
        d.module = j["module"].get<std::string>();
        d.supply = Supply::Imported;
        d.price = 1.0;  // set once every good is read: the parts at their prices
    }
    else if (j.contains("parts"))
    {
        why = "good '" + d.id + "': only a module has parts";
        return false;
    }
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
    if (!j.is_object() || !OnlyKnownKeys(j, { "recipes", "uses", "makes" }, why))
        return false;
    // A yard's modules (#472 slice 10): {"Mod.Laser": 3} is a recipe from the module's parts,
    // three an hour, so the parts are written once, with the good.
    if (j.contains("makes"))
    {
        if (!j["makes"].is_object())
        {
            why = "makes is an object of module: rate";
            return false;
        }
        for (auto it = j["makes"].begin(); it != j["makes"].end(); ++it)
        {
            const int g = IndexOf(r, it.key());
            if (g < 0 || !r.goods[(size_t)g].IsModule() || !it.value().is_number() ||
                it.value().get<double>() <= 0.0)
            {
                why = "makes '" + it.key() + "': a module of the registry, at a positive rate";
                return false;
            }
            Recipe rec;
            rec.in = r.goods[(size_t)g].parts;
            rec.out.push_back({ ResourceType::FromIndex(g), 1.0 });
            rec.rate = it.value().get<double>();
            ind.recipes.push_back(rec);
        }
    }
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
                             "beltRegrow", "haulEvery", "haulCargo", "haulMargin", "moduleMargin",
                             "haulKeep" },
                           why) ||
            !Number(m, "fee", k.fee, why) || !Number(m, "spread", k.spread, why) ||
            !Number(m, "elasticity", k.elasticity, why) || !Number(m, "floor", k.floor, why) ||
            !Number(m, "ceiling", k.ceiling, why) || !Number(m, "beltMined", k.beltMined, why) ||
            !Number(m, "beltRegrow", k.beltRegrow, why) ||
            !Number(m, "haulEvery", k.haulEvery, why) ||
            !Number(m, "haulCargo", k.haulCargo, why) ||
            !Number(m, "haulMargin", k.haulMargin, why) ||
            !Number(m, "moduleMargin", k.moduleMargin, why) ||
            !Number(m, "haulKeep", k.haulKeep, why))
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
    // A module's parts are goods of the registry, and its price follows theirs.
    for (const json& gj : data["goods"])
    {
        if (!gj.contains("parts"))
            continue;
        GoodDef& d = r.goods[(size_t)IndexOf(r, gj["id"].get<std::string>())];
        if (!Amounts(r, gj["parts"], d.parts, why) || d.parts.empty())
        {
            why = "good '" + d.id +
                  "': parts: " + (why.empty() ? "a module is made of something" : why);
            return false;
        }
        for (const auto& p : d.parts)
            if (r.goods[(size_t)p.first.Index()].IsModule())
            {
                why = "good '" + d.id + "': a module is made of goods, not of modules";
                return false;
            }
    }
    for (GoodDef& d : r.goods)
        if (d.IsModule())
        {
            double sum = 0.0;
            for (const auto& p : d.parts)
                sum += p.second * r.goods[(size_t)p.first.Index()].price;
            d.price = std::max(1.0, std::round(sum * r.rules.moduleMargin));
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

bool ModuleGood(const std::string& moduleId, ResourceType& out)
{
    for (int i = 0; i < Goods::Count(); i++)
        if (Goods::Def(ResourceType::FromIndex(i)).module == moduleId)
        {
            out = ResourceType::FromIndex(i);
            return true;
        }
    return false;
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

int Volume(ResourceType g)
{
    return std::max(1, Def(g).volume);
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
