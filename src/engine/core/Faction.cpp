#include "core/Faction.h"

#include "core/JsonKeys.h"

#include <nlohmann/json.hpp>
#include <fstream>
#include <utility>

using json = nlohmann::json;

namespace
{
// Everything factions.json says, as one value: a load builds a new one and swaps it in only
// when the whole file has read, so a file with a mistake leaves the registry as it was.
struct Registry
{
    std::vector<FactionDef>          factions;
    std::vector<std::vector<Stance>> relation;  // [a][b], kept symmetric
    Migration                        migration;
    int                              pirates = 0, police = 0, traders = 0;  // roles, by index
    float repHated = -50.0f, repHostile = -10.0f, repLiked = 10.0f, repAllied = 50.0f;
};

// The compiled order a save from before #355 wrote faction indices in.
const char* const LEGACY_ORDER[] = { "Independent", "TradersGuild", "Syndicate", "Pirates" };

int IndexOf(const Registry& r, const std::string& id)
{
    for (size_t i = 0; i < r.factions.size(); i++)
        if (r.factions[i].id == id)
            return (int)i;
    return -1;
}

// What a pair nobody named in the file is: allied with itself, hostile to the pirates,
// neutral otherwise.
Stance DefaultStance(const Registry& r, int a, int b)
{
    if (a == b)
        return Stance::Ally;
    if (a == r.pirates || b == r.pirates)
        return Stance::Hostile;
    return Stance::Neutral;
}

void ResetRelations(Registry& r)
{
    const int n = (int)r.factions.size();
    r.relation.assign((size_t)n, std::vector<Stance>((size_t)n, Stance::Neutral));
    for (int a = 0; a < n; a++)
        for (int b = 0; b < n; b++)
            r.relation[(size_t)a][(size_t)b] = DefaultStance(r, a, b);
}

// The four factions of the original game, for a process that loads no file.
Registry Builtin()
{
    Registry r;
    auto     add = [&](const char* id, const char* name, Color c, bool lawful, const char* kind)
    {
        FactionDef d;
        d.id = id;
        d.name = name;
        d.color = c;
        d.lawful = lawful;
        d.kind = kind;
        r.factions.push_back(d);
    };
    add("Independent", "Independent", Color{ 200, 200, 200, 255 }, true, "Independent");
    add("TradersGuild", "Traders Guild", Color{ 0, 228, 48, 255 }, true, "Major");
    add("Syndicate", "Syndicate", Color{ 230, 41, 55, 255 }, true, "Major");
    add("Pirates", "Pirates", Color{ 255, 161, 0, 255 }, false, "Pirate");
    r.pirates = 3;
    r.police = 1;
    r.traders = 1;
    ResetRelations(r);
    return r;
}

Registry    g_registry = Builtin();
std::string g_error;

const FactionDef& Def(FactionId f)
{
    return g_registry.factions[(size_t)f.Index()];
}

const std::pair<const char*, Stance> STANCE_NAMES[] = {
    { "War", Stance::War },         { "Hostile", Stance::Hostile },
    { "Neutral", Stance::Neutral }, { "Friendly", Stance::Friendly },
    { "Ally", Stance::Ally },
};

bool StanceFromString(const std::string& s, Stance& out)
{
    for (const auto& [name, stance] : STANCE_NAMES)
        if (s == name)
        {
            out = stance;
            return true;
        }
    return false;
}

bool IsNumber(const json& j, const char* key)
{
    return !j.contains(key) || j[key].is_number();
}

// One entry of `factions`. Every field but `id` has a default; every field present must be
// one the reader knows, of the type it expects.
bool ReadFaction(const json& j, FactionDef& d, std::string& error)
{
    if (!j.is_object())
    {
        error = "a faction is an object";
        return false;
    }
    if (!j.contains("id") || !j["id"].is_string() || j["id"].get<std::string>().empty())
    {
        error = "a faction has no 'id'";
        return false;
    }
    d.id = j["id"].get<std::string>();
    const std::string where = "faction '" + d.id + "': ";
    std::string       why;
    if (!OnlyKnownKeys(j, { "id", "name", "color", "lawful", "kind", "temperament" }, why))
    {
        error = where + why;
        return false;
    }
    if ((j.contains("name") && !j["name"].is_string()) ||
        (j.contains("kind") && !j["kind"].is_string()) ||
        (j.contains("lawful") && !j["lawful"].is_boolean()))
    {
        error = where + "'name' and 'kind' are strings, 'lawful' is true or false";
        return false;
    }
    d.name = j.value("name", d.id);
    d.kind = j.value("kind", std::string());
    d.lawful = j.value("lawful", true);
    if (j.contains("color"))
    {
        const json& c = j["color"];
        if (!c.is_array() || c.size() < 3 || c.size() > 4)
        {
            error = where + "'color' is [r, g, b]";
            return false;
        }
        for (const json& v : c)
            if (!v.is_number_integer() || v.get<int>() < 0 || v.get<int>() > 255)
            {
                error = where + "'color' is [r, g, b], each 0..255";
                return false;
            }
        d.color = Color{ (unsigned char)c[0].get<int>(), (unsigned char)c[1].get<int>(),
                         (unsigned char)c[2].get<int>(), 255 };
    }
    if (j.contains("temperament"))
    {
        const json& t = j["temperament"];
        if (!t.is_object() ||
            !OnlyKnownKeys(t, { "appetite", "capacity", "growth", "curiosity", "values" }, why))
        {
            error = where + "temperament: " + (why.empty() ? "not an object" : why);
            return false;
        }
        const json v = t.value("values", json::object());
        if (!v.is_object() || !OnlyKnownKeys(v, { "traffic", "ore", "salvage", "unclaimed" }, why))
        {
            error = where + "temperament.values: " + (why.empty() ? "not an object" : why);
            return false;
        }
        for (const char* k : { "appetite", "capacity", "growth", "curiosity" })
            if (!IsNumber(t, k))
            {
                error = where + "temperament." + k + " is a number";
                return false;
            }
        for (const char* k : { "traffic", "ore", "salvage", "unclaimed" })
            if (!IsNumber(v, k))
            {
                error = where + "temperament.values." + k + " is a number";
                return false;
            }
        Temperament& m = d.temperament;
        m.appetite = t.value("appetite", m.appetite);
        m.capacity = t.value("capacity", m.capacity);
        m.growth = t.value("growth", m.growth);
        m.curiosity = t.value("curiosity", m.curiosity);
        m.traffic = v.value("traffic", m.traffic);
        m.ore = v.value("ore", m.ore);
        m.salvage = v.value("salvage", m.salvage);
        m.unclaimed = v.value("unclaimed", m.unclaimed);
    }
    return true;
}

bool Parse(const json& data, Registry& r, std::string& error)
{
    std::string why;
    if (!data.is_object())
    {
        error = "not a JSON object";
        return false;
    }
    if (!OnlyKnownKeys(data, { "factions", "relations", "roles", "migration", "repTiers" }, why))
    {
        error = why;
        return false;
    }
    if (!data.contains("factions") || !data["factions"].is_array() || data["factions"].empty())
    {
        error = "'factions' is missing: a list of at least one faction";
        return false;
    }
    for (const json& j : data["factions"])
    {
        FactionDef d;
        if (!ReadFaction(j, d, error))
            return false;
        if (IndexOf(r, d.id) >= 0)
        {
            error = "faction '" + d.id + "' is listed twice";
            return false;
        }
        r.factions.push_back(d);
    }

    // The parts the world gives a faction to play. Every one is named, and named by an id in
    // the list: a misspelt role would otherwise make some other faction the pirates.
    if (!data.contains("roles") || !data["roles"].is_object() ||
        !OnlyKnownKeys(data["roles"], { "unaffiliated", "pirates", "police", "traders" }, why))
    {
        error = "roles: " +
                (why.empty() ? "missing -- { unaffiliated, pirates, police, traders }" : why);
        return false;
    }
    int               roleIndex[4] = { -1, -1, -1, -1 };
    const char* const ROLES[4] = { "unaffiliated", "pirates", "police", "traders" };
    for (int i = 0; i < 4; i++)
    {
        const json& roles = data["roles"];
        if (!roles.contains(ROLES[i]) || !roles[ROLES[i]].is_string())
        {
            error = std::string("roles: '") + ROLES[i] + "' is missing";
            return false;
        }
        const std::string id = roles[ROLES[i]].get<std::string>();
        roleIndex[i] = IndexOf(r, id);
        if (roleIndex[i] < 0)
        {
            error = std::string("roles: ") + ROLES[i] + " is '" + id + "', which is not a faction";
            return false;
        }
    }
    // A FactionId that was never set is the first faction, so that is the one which must own
    // what nobody owns.
    if (roleIndex[0] != 0)
    {
        error = "roles: the unaffiliated faction ('" + r.factions[(size_t)roleIndex[0]].id +
                "') must be listed first";
        return false;
    }
    r.pirates = roleIndex[1];
    r.police = roleIndex[2];
    r.traders = roleIndex[3];
    ResetRelations(r);

    if (data.contains("relations"))
    {
        if (!data["relations"].is_array())
        {
            error = "'relations' is a list of [faction, faction, stance]";
            return false;
        }
        for (const json& rel : data["relations"])
        {
            if (!rel.is_array() || rel.size() != 3 || !rel[0].is_string() || !rel[1].is_string() ||
                !rel[2].is_string())
            {
                error = "relations: " + rel.dump() + " is not [faction, faction, stance]";
                return false;
            }
            const int a = IndexOf(r, rel[0].get<std::string>());
            const int b = IndexOf(r, rel[1].get<std::string>());
            Stance    st = Stance::Neutral;
            if (a < 0 || b < 0)
            {
                error = "relations: " + rel.dump() + " names a faction that is not listed";
                return false;
            }
            if (!StanceFromString(rel[2].get<std::string>(), st))
            {
                error = "relations: " + rel.dump() +
                        " -- a stance is War, Hostile, Neutral, Friendly or Ally";
                return false;
            }
            r.relation[(size_t)a][(size_t)b] = st;
            r.relation[(size_t)b][(size_t)a] = st;
        }
    }

    // Where ships a system has too many of go, and how fast (#295).
    if (data.contains("migration"))
    {
        const json& m = data["migration"];
        if (!m.is_object() ||
            !OnlyKnownKeys(m, { "trader", "miner", "patrol", "pirate", "travel", "align" }, why))
        {
            error = "migration: " + (why.empty() ? "not an object" : why);
            return false;
        }
        for (auto it = m.begin(); it != m.end(); ++it)
            if (!it->is_number() || it->get<double>() < 0.0)
            {
                error = "migration." + it.key() + " is a number of seconds, at least 0";
                return false;
            }
        r.migration.trader = m.value("trader", r.migration.trader);
        r.migration.miner = m.value("miner", r.migration.miner);
        r.migration.patrol = m.value("patrol", r.migration.patrol);
        r.migration.pirate = m.value("pirate", r.migration.pirate);
        r.migration.travel = m.value("travel", r.migration.travel);
        r.migration.align = m.value("align", r.migration.align);
    }

    // Reputation tier thresholds.
    if (data.contains("repTiers"))
    {
        const json& t = data["repTiers"];
        if (!t.is_object() || !OnlyKnownKeys(t, { "hated", "hostile", "liked", "allied" }, why))
        {
            error = "repTiers: " + (why.empty() ? "not an object" : why);
            return false;
        }
        for (auto it = t.begin(); it != t.end(); ++it)
            if (!it->is_number())
            {
                error = "repTiers." + it.key() + " is a number";
                return false;
            }
        r.repHated = t.value("hated", r.repHated);
        r.repHostile = t.value("hostile", r.repHostile);
        r.repLiked = t.value("liked", r.repLiked);
        r.repAllied = t.value("allied", r.repAllied);
    }
    return true;
}
}  // namespace

std::string FactionName(FactionId faction)
{
    return Def(faction).name;
}

Color FactionColor(FactionId faction)
{
    return Def(faction).color;
}

namespace Factions
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
    std::string error;
    if (!Parse(data, r, error))
    {
        g_error = path + ": " + error;
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

FactionId Add(const FactionDef& def)
{
    FactionId existing;
    if (Find(def.id, existing))
        return existing;
    Registry& r = g_registry;
    r.factions.push_back(def);
    const int n = (int)r.factions.size();
    for (int a = 0; a < n - 1; a++)
        r.relation[(size_t)a].push_back(DefaultStance(r, a, n - 1));
    r.relation.emplace_back();
    for (int b = 0; b < n; b++)
        r.relation.back().push_back(DefaultStance(r, n - 1, b));
    return FactionId::FromIndex(n - 1);
}

int Count()
{
    return (int)g_registry.factions.size();
}

const FactionDef& Def(FactionId f)
{
    return ::Def(f);
}

void SetRelation(FactionId a, FactionId b, Stance stance)
{
    const size_t n = g_registry.factions.size();
    if ((size_t)a.Index() >= n || (size_t)b.Index() >= n || a == b)
        return;
    g_registry.relation[(size_t)a.Index()][(size_t)b.Index()] = stance;
    g_registry.relation[(size_t)b.Index()][(size_t)a.Index()] = stance;
}

const char* StanceName(Stance s)
{
    for (const auto& [name, stance] : STANCE_NAMES)
        if (s == stance)
            return name;
    return "Neutral";
}

bool ParseStance(const std::string& s, Stance& out)
{
    return StanceFromString(s, out);
}

bool Find(const std::string& id, FactionId& out)
{
    const int i = IndexOf(g_registry, id);
    if (i < 0)
        return false;
    out = FactionId::FromIndex(i);
    return true;
}

FactionId Unaffiliated()
{
    return FactionId();
}

FactionId Pirates()
{
    return FactionId::FromIndex(g_registry.pirates);
}

FactionId DefaultPolice()
{
    return FactionId::FromIndex(g_registry.police);
}

FactionId Traders()
{
    return FactionId::FromIndex(g_registry.traders);
}

bool FromLegacyIndex(int i, FactionId& out)
{
    const int n = (int)(sizeof(LEGACY_ORDER) / sizeof(LEGACY_ORDER[0]));
    return i >= 0 && i < n && Find(LEGACY_ORDER[i], out);
}

const std::string& Id(FactionId f)
{
    return Def(f).id;
}

bool IsLawful(FactionId f)
{
    return Def(f).lawful;
}

const std::string& Kind(FactionId f)
{
    return Def(f).kind;
}

std::vector<std::string> MajorIds()
{
    std::vector<std::string> out;
    for (FactionId f : All())
        if (Def(f).kind == "Major" && !(f == Pirates()))
            out.push_back(Def(f).id);
    return out;
}

const Temperament& TemperamentOf(FactionId f)
{
    return Def(f).temperament;
}

const Migration& MigrationRules()
{
    return g_registry.migration;
}

Stance Relation(FactionId a, FactionId b)
{
    return g_registry.relation[(size_t)a.Index()][(size_t)b.Index()];
}

RepTier TierOf(float rep)
{
    const Registry& r = g_registry;
    if (rep < r.repHated)
        return RepTier::Hated;
    if (rep < r.repHostile)
        return RepTier::Hostile;
    if (rep < r.repLiked)
        return RepTier::Neutral;
    if (rep < r.repAllied)
        return RepTier::Liked;
    return RepTier::Allied;
}

std::string TierName(RepTier t)
{
    switch (t)
    {
        case RepTier::Hated: return "Hated";
        case RepTier::Hostile: return "Hostile";
        case RepTier::Neutral: return "Neutral";
        case RepTier::Liked: return "Liked";
        case RepTier::Allied: return "Allied";
    }
    return "Neutral";
}

Color TierColor(RepTier t)
{
    switch (t)
    {
        case RepTier::Hated: return Color{ 230, 41, 55, 255 };      // red
        case RepTier::Hostile: return Color{ 255, 161, 0, 255 };    // orange
        case RepTier::Neutral: return Color{ 200, 200, 200, 255 };  // gray
        case RepTier::Liked: return Color{ 120, 210, 130, 255 };    // light green
        case RepTier::Allied: return Color{ 0, 228, 48, 255 };      // green
    }
    return WHITE;
}
}  // namespace Factions
