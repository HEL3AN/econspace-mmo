#include "sim/Overview.h"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace Overview
{
namespace
{
struct Named
{
    Filter      filter;
    const char* label;
};

const Named FILTERS[] = {
    { Filter::All, "All" },         { Filter::Destinations, "Go" }, { Filter::Ships, "Ships" },
    { Filter::Features, "Places" }, { Filter::Hostile, "Hostile" },
};

bool Passes(Filter f, const Proto::EntitySnapshot& e, bool hostile)
{
    switch (f)
    {
        case Filter::All: return true;
        case Filter::Hostile: return hostile;
        case Filter::Destinations:
            return e.kind == Proto::EntityKind::Station || e.kind == Proto::EntityKind::Gate;
        case Filter::Ships:
            return e.kind == Proto::EntityKind::Npc || e.kind == Proto::EntityKind::PlayerShip;
        case Filter::Features:
            return e.kind == Proto::EntityKind::Star || e.kind == Proto::EntityKind::Planet ||
                   e.kind == Proto::EntityKind::Field || e.kind == Proto::EntityKind::Nebula ||
                   e.kind == Proto::EntityKind::Derelict;
    }
    return true;
}

std::string Lower(std::string s)
{
    for (char& c : s)
        c = (char)std::tolower((unsigned char)c);
    return s;
}
}  // namespace

const char* Label(Filter f)
{
    for (const Named& n : FILTERS)
        if (n.filter == f)
            return n.label;
    return "All";
}

const std::vector<Filter>& AllFilters()
{
    static const std::vector<Filter> all = { Filter::All, Filter::Destinations, Filter::Ships,
                                             Filter::Features, Filter::Hostile };
    return all;
}

const char* KindWord(const Proto::EntitySnapshot& e)
{
    switch (e.kind)
    {
        case Proto::EntityKind::Star: return "star";
        case Proto::EntityKind::Planet: return "planet";
        case Proto::EntityKind::Station: return "station";
        case Proto::EntityKind::Field: return "belt";
        case Proto::EntityKind::Gate: return "gate";
        case Proto::EntityKind::Nebula: return "nebula";
        case Proto::EntityKind::Derelict: return "wreck";
        case Proto::EntityKind::PlayerShip: return "pilot";
        case Proto::EntityKind::Npc:
            // An NPC by what it does, which is what a player decides on.
            switch (e.role)
            {
                case 0: return "trader";
                case 1: return "miner";
                case 2: return "police";
                case 3: return "pirate";
                case 4: return "warship";
                default: return "ship";
            }
        case Proto::EntityKind::Unknown: break;
    }
    return "object";
}

std::vector<Row> Build(const std::vector<Proto::EntitySnapshot>& entities, Vector2 from,
                       Filter filter, Sort sort, const IsHostile& hostile)
{
    std::vector<Row> rows;
    rows.reserve(entities.size());
    for (const Proto::EntitySnapshot& e : entities)
    {
        const bool h = hostile ? hostile(e) : false;
        if (!Passes(filter, e, h))
            continue;
        Row r;
        r.entity = &e;
        r.distance = std::hypot(e.pos.x - from.x, e.pos.y - from.y);
        r.hostile = h;
        r.kind = KindWord(e);
        r.name = e.name.empty() ? r.kind : e.name;
        rows.push_back(r);
    }

    // Stable, with the id as the last word, so two rows that tie never swap places between
    // frames -- a list that reshuffles under the cursor is a list you cannot click.
    std::stable_sort(rows.begin(), rows.end(),
                     [sort](const Row& a, const Row& b)
                     {
                         if (a.hostile != b.hostile)
                             return a.hostile;  // the reason to change plan, first
                         switch (sort)
                         {
                             case Sort::Name:
                             {
                                 const std::string la = Lower(a.name), lb = Lower(b.name);
                                 if (la != lb)
                                     return la < lb;
                                 break;
                             }
                             case Sort::Kind:
                                 if (a.kind != b.kind)
                                     return a.kind < b.kind;
                                 if (a.distance != b.distance)
                                     return a.distance < b.distance;
                                 break;
                             case Sort::Distance:
                                 if (a.distance != b.distance)
                                     return a.distance < b.distance;
                                 break;
                         }
                         return a.entity->id < b.entity->id;
                     });
    return rows;
}

}  // namespace Overview
