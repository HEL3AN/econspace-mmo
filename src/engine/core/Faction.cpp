#include "core/Faction.h"

#include <nlohmann/json.hpp>
#include <fstream>

using json = nlohmann::json;

namespace
{
constexpr int N = 4;  // number of factions (= size of enum FactionId)

// Default values (used until factions.json is loaded).
std::string g_id[N] = { "Independent", "TradersGuild", "Syndicate", "Pirates" };
std::string g_name[N] = { "Independent", "Traders Guild", "Syndicate", "Pirates" };
Color       g_color[N] = { Color{ 200, 200, 200, 255 }, Color{ 0, 228, 48, 255 },
                           Color{ 230, 41, 55, 255 }, Color{ 255, 161, 0, 255 } };
bool        g_lawful[N] = { true, true, true, false };
std::string g_kind[N] = { "Independent", "Major", "Major", "Pirate" };

// Relations matrix. By default: hostile toward pirates, neutral among each other,
// allied with oneself.
Stance g_relation[N][N];
bool   g_relationsInit = false;

// Reputation tier thresholds (defaults).
float g_repHated = -50.0f;
float g_repHostile = -10.0f;
float g_repLiked = 10.0f;
float g_repAllied = 50.0f;

void InitDefaultRelations()
{
    for (int a = 0; a < N; a++)
        for (int b = 0; b < N; b++)
        {
            if (a == b)
                g_relation[a][b] = Stance::Ally;
            else if (a == (int)FactionId::Pirates || b == (int)FactionId::Pirates)
                g_relation[a][b] = Stance::Hostile;
            else
                g_relation[a][b] = Stance::Neutral;
        }
    g_relationsInit = true;
}

// Maps a string (id or legacy name) to a FactionId index.
int IndexFromString(const std::string& s)
{
    if (s == "TradersGuild" || s == "Traders Guild")
        return (int)FactionId::TradersGuild;
    if (s == "Syndicate")
        return (int)FactionId::Syndicate;
    if (s == "Pirates")
        return (int)FactionId::Pirates;
    return (int)FactionId::Independent;
}

Stance StanceFromString(const std::string& s)
{
    if (s == "War")
        return Stance::War;
    if (s == "Hostile")
        return Stance::Hostile;
    if (s == "Friendly")
        return Stance::Friendly;
    if (s == "Ally")
        return Stance::Ally;
    return Stance::Neutral;
}
}  // namespace

std::string FactionName(FactionId faction)
{
    return g_name[(int)faction];
}

Color FactionColor(FactionId faction)
{
    return g_color[(int)faction];
}

FactionId FactionFromString(const std::string& name)
{
    return (FactionId)IndexFromString(name);
}

namespace Factions
{
void Load(const std::string& path)
{
    if (!g_relationsInit)
        InitDefaultRelations();

    std::ifstream f(path);
    if (!f.is_open())
        return;  // no file — defaults stay in effect
    json data = json::parse(f, nullptr, false);
    if (data.is_discarded())
        return;

    // Faction properties (map by id to the fixed enum indices).
    if (data.contains("factions"))
        for (const json& j : data["factions"])
        {
            std::string id = j.value("id", std::string());
            int         i = IndexFromString(id);
            if (i < 0 || i >= N)
                continue;
            if (!id.empty())
                g_id[i] = id;
            g_name[i] = j.value("name", g_name[i]);
            g_lawful[i] = j.value("lawful", g_lawful[i]);
            g_kind[i] = j.value("kind", g_kind[i]);
            if (j.contains("color") && j["color"].size() >= 3)
                g_color[i] = Color{ (unsigned char)j["color"][0], (unsigned char)j["color"][1],
                                    (unsigned char)j["color"][2], 255 };
        }

    // Reputation tier thresholds.
    if (data.contains("repTiers"))
    {
        const json& t = data["repTiers"];
        g_repHated = (float)t.value("hated", g_repHated);
        g_repHostile = (float)t.value("hostile", g_repHostile);
        g_repLiked = (float)t.value("liked", g_repLiked);
        g_repAllied = (float)t.value("allied", g_repAllied);
    }

    // Relations (stored symmetrically).
    if (data.contains("relations"))
        for (const json& r : data["relations"])
            if (r.is_array() && r.size() >= 3)
            {
                int    a = IndexFromString(r[0]);
                int    b = IndexFromString(r[1]);
                Stance st = StanceFromString(r[2]);
                if (a >= 0 && a < N && b >= 0 && b < N)
                {
                    g_relation[a][b] = st;
                    g_relation[b][a] = st;
                }
            }
}

int Count()
{
    return N;
}

std::string Id(FactionId f)
{
    return g_id[(int)f];
}

bool IsLawful(FactionId f)
{
    return g_lawful[(int)f];
}

std::string Kind(FactionId f)
{
    return g_kind[(int)f];
}

Stance Relation(FactionId a, FactionId b)
{
    if (!g_relationsInit)
        InitDefaultRelations();
    return g_relation[(int)a][(int)b];
}

RepTier TierOf(float rep)
{
    if (rep < g_repHated)
        return RepTier::Hated;
    if (rep < g_repHostile)
        return RepTier::Hostile;
    if (rep < g_repLiked)
        return RepTier::Neutral;
    if (rep < g_repAllied)
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
