#pragma once

#include "core/Faction.h"

#include <string>
#include <vector>

// Ship parameters. Change when switching to another ship.
struct ShipStats
{
    float thrustPower;    // engine thrust (stabilizer-off mode)
    float turnSpeed;      // turn rate, rad/sec
    float maxSpeed;       // top speed
    float rcsAccel;       // stabilizer power
    int   cargoCapacity;  // cargo hold capacity
    float miningRate;     // mining rate, units/sec
};

// A ship type in the hangar: name, price, base stats.
struct ShipType
{
    std::string name;
    double      price;
    ShipStats   stats;
};

// Catalog of available ships. The first one is the starter.
const std::vector<ShipType>& GetShipCatalog();

// What a station charges for a ship, as a multiple of the catalog price, given the buyer's
// standing with the station's owner. One function because three places need the same number
// (#109): the server charges it, the hangar screen shows it, and an agent quotes it before
// deciding whether it can afford one. Two copies of a price are how a button says one thing
// and the account is charged another.
float ShipPriceMultiplier(RepTier standing);

// What a station pays for cargo, as a multiple of its gross, given the seller's standing.
float SellPriceMultiplier(RepTier standing);

// How a station scales the money and reputation its missions offer, given the standing of
// the player reading its board.
float MissionRewardMultiplier(RepTier standing);

// The three tables move together and are monotonic in standing (#219): a worse tier is never
// a better deal. Hated once fell through to list price -- cheaper than Hostile -- and only
// the dock refusing Hated players hid it.
