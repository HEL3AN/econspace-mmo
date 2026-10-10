#include "core/StationList.h"

#include <algorithm>

namespace StationList
{

std::vector<MarketRow> Market(const std::vector<float>& prices, const std::vector<int>& cargo,
                              float tradeBonus, RepTier standing)
{
    const double           mul = (double)tradeBonus * SellPriceMultiplier(standing);
    std::vector<MarketRow> rows;
    int                    i = 0;
    for (ResourceType type : AllResourceTypes())
    {
        MarketRow r;
        r.type = type;
        r.name = ResourceName(type);
        r.price = i < (int)prices.size() ? prices[i] : 0.0f;
        r.hold = i < (int)cargo.size() ? std::max(0, cargo[i]) : 0;
        r.perUnit = r.price * mul;
        r.worth = r.perUnit * r.hold;
        rows.push_back(r);
        i++;
    }
    return rows;
}

std::vector<HangarRow> Hangar(const std::vector<ShipType>& catalog, const std::vector<int>& owned,
                              int current, double money, int cargoUsed, RepTier standing)
{
    const float            mul = ShipPriceMultiplier(standing);
    std::vector<HangarRow> rows;
    for (int i = 0; i < (int)catalog.size(); i++)
    {
        const ShipType& t = catalog[i];
        HangarRow       r;
        r.index = i;
        r.name = t.name;
        r.speed = t.stats.maxSpeed;
        r.cargo = t.stats.cargoCapacity;
        r.mining = t.stats.miningRate;
        r.price = t.price * mul;
        const bool mine = i == 0 || std::find(owned.begin(), owned.end(), i) != owned.end();
        r.berth = i == current ? Berth::Current : mine ? Berth::Owned : Berth::ForSale;
        r.affordable = r.berth != Berth::ForSale || money >= r.price;
        r.holdFits = cargoUsed <= t.stats.cargoCapacity;
        rows.push_back(r);
    }
    return rows;
}

}  // namespace StationList
