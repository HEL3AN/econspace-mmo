#pragma once

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
