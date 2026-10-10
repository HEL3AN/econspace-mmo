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
inline constexpr int FACTION_COUNT = 4;

// How a faction acts on the galaxy (#231): the same machinery for every faction, with only
// this differing, so a new faction is data. A faction reaches into a neighbouring system
// when what it values there outweighs the risk by its own appetite for risk.
struct Temperament
{
    float appetite = 0.0f;  // how bold: 0 never reaches out; 1 takes an even bet
    float capacity = 4.0f;  // the strength (ships) one of its systems sustains
    float growth = 0.1f;    // how fast its strength in a system it holds recovers, per period
    // What it is after, as weights on what a system offers (each 0..1).
    float traffic = 0.0f;    // ships and trade passing through: prey, or customers
    float ore = 0.0f;        // belts to mine
    float salvage = 0.0f;    // wrecks, ruins, finds
    float unclaimed = 0.0f;  // a system nobody holds
    // What finding out is worth to it (#295): the value of surveying a system it knows
    // nothing about. 0 never looks further than it can see.
    float curiosity = 0.0f;
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

int                Count();
std::string        Id(FactionId f);  // string id (as used in station data)
bool               IsLawful(FactionId f);
std::string        Kind(FactionId f);
Stance             Relation(FactionId a, FactionId b);
const Temperament& TemperamentOf(FactionId f);

// Player reputation tiers.
RepTier     TierOf(float rep);
std::string TierName(RepTier t);
Color       TierColor(RepTier t);
}  // namespace Factions
