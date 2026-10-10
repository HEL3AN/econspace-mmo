#pragma once

#include "core/Faction.h"
#include "economy/Resource.h"
#include "entities/ShipType.h"

#include <string>
#include <vector>

// What a station's market and hangar windows list (#297), with no drawing in it -- the half a
// test can hold, as MissionList is for the missions window. It reads what the client was
// sent (the snapshot's prices, the hold, the ships owned) and the same price tables the
// server charges by, so a number in a window is the number the account will see.
namespace StationList
{

// One commodity at the station the ship is docked at.
struct MarketRow
{
    ResourceType type = ResourceType::Iron;
    std::string  name;
    float        price = 0.0f;  // the station's price per unit, before standing and skill
    int          hold = 0;      // how much of it the ship carries
    // What one unit pays this pilot now: the price times the trading skill's bonus times
    // what the station's owner thinks of them -- the server's sum for a sale. A sale lowers
    // the price for the next one, so this is the next unit, not every unit after it.
    double perUnit = 0.0;
    double worth = 0.0;  // the whole hold of it at perUnit: what "Sell all" pays
};

// Every commodity, in AllResourceTypes order; `prices` and `cargo` are the snapshot's lists
// in that order, and a missing entry is 0.
std::vector<MarketRow> Market(const std::vector<float>& prices, const std::vector<int>& cargo,
                              float tradeBonus, RepTier standing);

// Whose a hull in the catalog is.
enum class Berth
{
    Current,  // the ship being flown
    Owned,    // bought before: switching to it is free
    ForSale,
};

struct HangarRow
{
    int         index = 0;  // in the catalog: what buy_ship and refit take
    std::string name;
    float       speed = 0.0f;
    int         cargo = 0;
    float       mining = 0.0f;
    double      price = 0.0;  // what this station charges this pilot (ShipPriceMultiplier)
    Berth       berth = Berth::ForSale;
    bool        affordable = true;  // for sale, and the money is there
    // The hold would take what the ship carries now. The server refuses a refit into a
    // smaller hold than the cargo (#219), and a button that is refused says less than one
    // that says why first.
    bool holdFits = true;
};

// The catalog as this pilot sees it here. The starter (index 0) is always owned, as the
// server has it.
std::vector<HangarRow> Hangar(const std::vector<ShipType>& catalog, const std::vector<int>& owned,
                              int current, double money, int cargoUsed, RepTier standing);

}  // namespace StationList
