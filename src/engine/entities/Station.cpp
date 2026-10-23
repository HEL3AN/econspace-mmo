#include "entities/Station.h"
#include "render/Textures.h"

std::string StationRoleName(StationRole role)
{
    switch (role)
    {
        case StationRole::TradeHub: return "Trade Hub";
        case StationRole::MiningOutpost: return "Mining Outpost";
        case StationRole::Shipyard: return "Shipyard";
        case StationRole::Military: return "Military";
    }
    return "Station";
}

static const char* ArchetypeIdForStationRole(StationRole role)
{
    switch (role)
    {
        case StationRole::TradeHub: return "station.trade_hub";
        case StationRole::MiningOutpost: return "station.mining_outpost";
        case StationRole::Shipyard: return "station.shipyard";
        case StationRole::Military: return "station.military";
    }
    return "station.trade_hub";
}

std::string StationRoleId(StationRole role)
{
    switch (role)
    {
        case StationRole::TradeHub: return "TradeHub";
        case StationRole::MiningOutpost: return "MiningOutpost";
        case StationRole::Shipyard: return "Shipyard";
        case StationRole::Military: return "Military";
    }
    return "TradeHub";
}

bool ParseStationRole(const std::string& s, StationRole& out)
{
    if (s == "TradeHub")
        out = StationRole::TradeHub;
    else if (s == "MiningOutpost")
        out = StationRole::MiningOutpost;
    else if (s == "Shipyard")
        out = StationRole::Shipyard;
    else if (s == "Military")
        out = StationRole::Military;
    else
        return false;
    return true;
}

StationRole StationRoleFromString(const std::string& s)
{
    StationRole role = StationRole::TradeHub;
    ParseStationRole(s, role);
    return role;
}

Station::Station(Vector2 pos, float size, std::string name, FactionId faction, StationRole role)
    : Entity(pos, size, LIGHTGRAY, EntityKind::Station), name_(std::move(name)), faction_(faction),
      role_(role)
{
    SetArchetype(ArchetypeIdForStationRole(role));
}

Render::Item Station::Describe() const
{
    Render::Item it = Entity::Describe();
    // The livery of the people who built it (#516) -- its owner's culture, or the owner
    // itself for a faction that has none. An art palette, not allegiance (#117): whether a
    // Guild hub is friend or target is the overview's to say.
    if (faction_.Index() < Factions::Count())
    {
        const FactionDef& f = Factions::Def(faction_);
        it.livery = f.culture.empty() ? f.id : f.culture;
    }
    return it;
}
