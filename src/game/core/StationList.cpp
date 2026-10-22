#include "core/StationList.h"

#include <algorithm>

namespace StationList
{

std::vector<MarketRow> Market(const MarketInput& in)
{
    const double           mul = (double)in.tradeBonus * SellPriceMultiplier(in.standing);
    const double           markup = 1.0 / std::min(1.0f, SellPriceMultiplier(in.standing));
    const double           fee = in.fee < 0.0 ? Goods::Rules().fee : in.fee;
    std::vector<MarketRow> rows;
    auto                   at = [](const auto& v, size_t i) { return i < v.size() ? v[i] : 0; };
    for (ResourceType type : AllResourceTypes())
    {
        const size_t i = (size_t)type.Index();
        MarketRow    r;
        r.type = type;
        r.name = ResourceName(type);
        r.tier = Goods::Def(type).tier;
        r.price = at(in.prices, i);
        r.ask = at(in.asks, i);
        r.stock = std::max(0, at(in.stock, i));
        r.hold = std::max(0, at(in.cargo, i));
        const double paid = r.ask > 0.0f ? std::min(r.price * mul, (double)r.ask) : r.price * mul;
        r.perUnit = paid * (1.0 - fee);
        r.worth = r.perUnit * r.hold;
        r.buyUnit = r.ask * markup * (1.0 + fee);
        r.canBuy = std::min(r.stock, std::max(0, in.holdFree));
        if (r.buyUnit > 0.0)
            r.canBuy = std::min(r.canBuy, (int)(in.money / r.buyUnit));
        rows.push_back(r);
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
        r.turn = t.stats.turnSpeed;
        r.hull = t.stats.hull;
        r.damage = t.stats.damage;
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
