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
