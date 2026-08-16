#pragma once

#include "entities/Entity.h"
#include "core/Faction.h"

// Station role — determines its services and the bias of mission generation.
enum class StationRole
{
    TradeHub,
    MiningOutpost,
    Shipyard,
    Military
};

std::string StationRoleName(StationRole role);
StationRole StationRoleFromString(const std::string& s);

// Space station: a hub with a screen (market, hangar). Belongs to a faction.
class Station : public Entity
{
public:
    Station(Vector2 pos, float size, std::string name, FactionId faction, StationRole role);

    std::unique_ptr<Entity> Clone() const override { return std::make_unique<Station>(*this); }
    std::string             GetName() const override { return name_; }

    FactionId   GetFaction() const { return faction_; }
    StationRole GetRole() const { return role_; }

private:
    std::string name_;
    FactionId   faction_;
    StationRole role_;
};
