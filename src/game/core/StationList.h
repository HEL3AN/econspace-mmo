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

// One good at the station the ship is docked at (#343).
struct MarketRow
{
    ResourceType type;
    std::string  name;
    int          tier = 0;
    float        price = 0.0f;  // what the market pays for a unit, before standing, skill and fee
    float        ask = 0.0f;    // what it asks for one, before standing and fee
    int          stock = 0;     // how many it holds
    int          hold = 0;      // how much of it the ship carries
    // What one unit pays this pilot now: the price times the trading skill's bonus times what
    // the station's owner thinks of them, never above the ask, less the fee -- the server's
    // sum for a sale. A sale lowers the price for the next one, so this is the next unit, not
    // every unit after it.
    double perUnit = 0.0;
    double worth = 0.0;  // the whole hold of it at perUnit: about what "Sell all" pays
    // What the next unit costs this pilot, the fee included, and about how many the stock,
    // the hold and the money allow -- each unit bought raises the price of the next, so the
    // server may sell a few fewer.
    double buyUnit = 0.0;
    int    canBuy = 0;
};

// The snapshot's market and what the pilot brings to it.
struct MarketInput
{
    std::vector<float> prices, asks;  // per good, AllResourceTypes order; missing reads 0
    std::vector<int>   stock, cargo;
    float              tradeBonus = 1.0f;
    RepTier            standing = RepTier::Neutral;
    double             money = 0.0;
    int                holdFree = 0;
    // The share of a trade the market takes: its system's holder's tax (#431 slice 5); below
    // zero, the standard fee.
    double fee = -1.0;
};

// Every good, in AllResourceTypes order.
std::vector<MarketRow> Market(const MarketInput& in);

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
    float       turn = 0.0f;    // rad/s
    float       hull = 0.0f;    // structure, before the shields
    float       damage = 0.0f;  // a shot of its guns
    double      price = 0.0;    // what this station charges this pilot (ShipPriceMultiplier)
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
