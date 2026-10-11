#include "entities/Defence.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdio>

bool Defence::IsDefault() const
{
    return *this == Defence();
}

bool Defence::operator==(const Defence& o) const
{
    return engage == o.engage && threshold == o.threshold && players == o.players &&
           factions == o.factions;
}

bool Defence::Allows(const std::string& player) const
{
    return std::find(players.begin(), players.end(), player) != players.end();
}

bool Defence::Allows(FactionId faction) const
{
    return std::find(factions.begin(), factions.end(), Factions::Id(faction)) != factions.end();
}

const char* EngageName(Engage e)
{
    switch (e)
    {
        case Engage::Hold: return "hold";
        case Engage::Return: return "return";
        case Engage::Enemies: return "enemies";
        case Engage::Strangers: return "strangers";
    }
    return "return";
}

const char* EngageLabel(Engage e)
{
    switch (e)
    {
        case Engage::Hold: return "Hold fire";
        case Engage::Return: return "Return fire";
        case Engage::Enemies: return "Enemies on sight";
        case Engage::Strangers: return "Everyone not on the list";
    }
    return "Return fire";
}

bool EngageFrom(const std::string& name, Engage& out)
{
    for (Engage e : { Engage::Hold, Engage::Return, Engage::Enemies, Engage::Strangers })
        if (name == EngageName(e))
        {
            out = e;
            return true;
        }
    return false;
}

std::string DefenceProblem(const Defence& d)
{
    if (!(d.threshold >= Defence::THRESHOLD_MIN && d.threshold <= Defence::THRESHOLD_MAX))
        return "the threshold is a standing from -100 to 100";
    if ((int)d.players.size() > Defence::LIST_MAX || (int)d.factions.size() > Defence::LIST_MAX)
        return "an allow-list holds at most " + std::to_string(Defence::LIST_MAX) + " names";
    for (const std::string& p : d.players)
        if (p.empty() || (int)p.size() > Defence::NAME_LENGTH_MAX)
            return "a player's name on the allow-list is 1 to " +
                   std::to_string(Defence::NAME_LENGTH_MAX) + " characters";
    FactionId f;
    for (const std::string& id : d.factions)
        if (!Factions::Find(id, f))
            return "there is no faction '" + id.substr(0, Defence::NAME_LENGTH_MAX) + "'";
    return std::string();
}

std::string DefenceCost(Engage engage, bool lawful, FactionId law)
{
    if (engage == Engage::Hold || engage == Engage::Return)
        return "It only fires back: that is no crime anywhere.";
    if (!lawful)
        return "No faction's law holds here: firing first costs nothing but the target's "
               "goodwill.";
    char buf[256];
    std::snprintf(buf, sizeof(buf),
                  "%s law holds here. Each hit on someone innocent is your crime: -%.1f "
                  "standing and %.0f cr bounty with %s, -%.0f and %.0f more for a kill.",
                  FactionName(law).c_str(), Crime::HIT_STANDING, Crime::HIT_BOUNTY,
                  FactionName(law).c_str(), Crime::KILL_STANDING, Crime::KILL_BOUNTY);
    std::string out = buf;
    out += engage == Engage::Strangers
               ? " Ships of every faction not on the list are fired on too, and a lawful one "
                 "charges you the same, wherever it is."
               : " Ships of a lawful faction you stand below the threshold with are fired on "
                 "too, and it charges you the same, wherever it is.";
    return out;
}

nlohmann::json DefenceJson(const Defence& d)
{
    nlohmann::json j = nlohmann::json::object();
    if (d.engage != Engage::Return)
        j["engage"] = EngageName(d.engage);
    if (d.threshold != Defence().threshold)
        j["threshold"] = d.threshold;
    if (!d.players.empty())
        j["players"] = d.players;
    if (!d.factions.empty())
        j["factions"] = d.factions;
    return j;
}

bool DefenceFrom(const nlohmann::json& j, Defence& out)
{
    if (!j.is_object())
        return false;
    Defence d;
    for (auto it = j.begin(); it != j.end(); ++it)
    {
        const nlohmann::json& v = it.value();
        if (it.key() == "engage")
        {
            if (!v.is_string() || !EngageFrom(v.get<std::string>(), d.engage))
                return false;
        }
        else if (it.key() == "threshold")
        {
            if (!v.is_number())
                return false;
            d.threshold = v.get<float>();
        }
        else if (it.key() == "players" || it.key() == "factions")
        {
            // Read no further than a list may be long: more is not a list anyone set.
            if (!v.is_array() || (int)v.size() > Defence::LIST_MAX)
                return false;
            std::vector<std::string>& list = it.key() == "players" ? d.players : d.factions;
            for (const nlohmann::json& n : v)
            {
                if (!n.is_string() || n.get_ref<const std::string&>().size() > 256)
                    return false;
                list.push_back(n.get<std::string>());
            }
        }
        else
            return false;
    }
    out = std::move(d);
    return true;
}

std::string AllowListText(const Defence& d)
{
    std::string out;
    for (const std::string& f : d.factions)
        out += (out.empty() ? "" : ", ") + f;
    for (const std::string& p : d.players)
        out += (out.empty() ? "" : ", ") + p;
    return out;
}

namespace
{
std::string Lower(std::string s)
{
    for (char& c : s)
        if (c >= 'A' && c <= 'Z')
            c = (char)(c - 'A' + 'a');
    return s;
}
}  // namespace

void ReadAllowList(const std::string& text, Defence& d)
{
    d.players.clear();
    d.factions.clear();
    size_t at = 0;
    while (at <= text.size())
    {
        size_t comma = text.find(',', at);
        if (comma == std::string::npos)
            comma = text.size();
        std::string name = text.substr(at, comma - at);
        at = comma + 1;
        const size_t a = name.find_first_not_of(" \t");
        const size_t b = name.find_last_not_of(" \t");
        if (a == std::string::npos)
            continue;
        name = name.substr(a, b - a + 1);
        std::string faction;
        for (FactionId f : Factions::All())
            if (Lower(name) == Lower(Factions::Id(f)) || Lower(name) == Lower(FactionName(f)))
                faction = Factions::Id(f);
        std::vector<std::string>& list = faction.empty() ? d.players : d.factions;
        const std::string&        entry = faction.empty() ? name : faction;
        if (std::find(list.begin(), list.end(), entry) == list.end())
            list.push_back(entry);
    }
}
