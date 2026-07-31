#include "entities/ShipType.h"

const std::vector<ShipType>& GetShipCatalog()
{
    // thrust, turn, maxSpeed, rcs, cargo, miningRate
    static const std::vector<ShipType> catalog = {
        { "Scout",   0.0,    { 340.0f, 3.2f, 440.0f, 600.0f, 50, 3.0f } },
        { "Courier", 2500.0, { 480.0f, 4.2f, 620.0f, 820.0f, 35, 2.0f } },
        { "Hauler",  3000.0, { 220.0f, 2.2f, 300.0f, 380.0f, 200, 2.0f } },
        { "Miner",   4200.0, { 260.0f, 2.6f, 360.0f, 460.0f, 120, 7.0f } },
    };
    return catalog;
}
