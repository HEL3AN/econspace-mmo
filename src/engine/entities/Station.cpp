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

Station::Station(Vector2 pos, float size, std::string name, FactionId faction, StationRole role)
    : Entity(pos, size, FactionColor(faction), EntityKind::Station), name_(std::move(name)),
      faction_(faction), role_(role)
{
    SetArchetype(Archetypes::Find(ArchetypeIdForStationRole(role)));
}
