#pragma once

#include "raylib.h"
#include <string>

// The game's factions. Stations and NPC ships belong to them; the player has a
// reputation with each faction. Properties and relations are loaded from
// data/factions.json (the enum order pins the indices); without the file,
// reasonable defaults apply.
enum class FactionId
{
    Independent,
    TradersGuild,
    Syndicate,
    Pirates
};

// Stance between two factions (for combat/hostility logic).
enum class Stance
{
    War,
    Hostile,
    Neutral,
    Friendly,
    Ally
};

// Player's reputation tier with a faction (by numeric thresholds from factions.json).
enum class RepTier
{
    Hated,
    Hostile,
    Neutral,
    Liked,
    Allied
};

std::string FactionName(FactionId faction);
Color       FactionColor(FactionId faction);
FactionId   FactionFromString(const std::string& name);

// Faction registry: data and relations from JSON.
namespace Factions
{
// Loads properties and relations from factions.json. Optional — defaults exist.
void Load(const std::string& path);

int         Count();
std::string Id(FactionId f);  // string id (as used in station data)
bool        IsLawful(FactionId f);
std::string Kind(FactionId f);
Stance      Relation(FactionId a, FactionId b);

// Player reputation tiers.
RepTier     TierOf(float rep);
std::string TierName(RepTier t);
Color       TierColor(RepTier t);
}  // namespace Factions
