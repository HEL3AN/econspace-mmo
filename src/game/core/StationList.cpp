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
        // A module nobody has put on this shelf, and none aboard, is no row (#566): there are
        // ten of them, and most markets have none.
        if (Goods::Def(type).IsModule() && r.stock <= 0 && r.hold <= 0)
            continue;
        const double paid = r.ask > 0.0f ? std::min(r.price * mul, (double)r.ask) : r.price * mul;
        r.perUnit = paid * (1.0 - fee);
        r.worth = r.perUnit * r.hold;
        r.buyUnit = r.ask * markup * (1.0 + fee);
        r.canBuy = std::min(r.stock, std::max(0, in.holdFree) / Goods::Volume(type));
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
    auto                   fill = [&](HangarRow& r, int hull)
    {
        const ShipType& t = catalog[(size_t)hull];
        r.index = hull;
        r.name = t.name;
        r.speed = t.stats.maxSpeed;
        r.cargo = t.stats.cargoCapacity;
        r.mining = t.stats.miningRate;
        r.turn = t.stats.turnSpeed;
        r.hull = t.stats.hull;
        r.damage = t.stats.damage;
        r.price = t.price * mul;
        // Each ship has a hold of its own (#472 slice 8): what this one carries stays aboard it.
        r.holdFits = true;
        (void)cargoUsed;
    };
    // The ships the account owns, one row each (#572) -- two of a hull are two rows -- and then
    // every hull there is to buy, as many of each as the pilot can pay for.
    std::vector<int> mine = owned;
    if (std::find(mine.begin(), mine.end(), 0) == mine.end())
        mine.insert(mine.begin(), 0);  // the starter is always the pilot's
    for (int key : mine)
    {
        const int hull = ShipHull(key);
        if (hull < 0 || hull >= (int)catalog.size())
            continue;
        HangarRow r;
        fill(r, hull);
        r.key = key;
        if (key / SHIP_STRIDE > 0)
            r.name += " #" + std::to_string(key / SHIP_STRIDE + 1);
        r.berth = key == current ? Berth::Current : Berth::Owned;
        r.affordable = true;
        rows.push_back(r);
    }
    for (int i = 0; i < (int)catalog.size(); i++)
    {
        HangarRow r;
        fill(r, i);
        r.berth = Berth::ForSale;
        r.affordable = money >= r.price;
        rows.push_back(r);
    }
    return rows;
}

}  // namespace StationList
